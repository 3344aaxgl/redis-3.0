/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"
#include "lzf.h"    /* LZF compression library */
#include "zipmap.h"
#include "endianconv.h"

#include <math.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/stat.h>

static int rdbWriteRaw(rio *rdb, void *p, size_t len) {
    if (rdb && rioWrite(rdb,p,len) == 0)//写数据
        return -1;
    return len;
}

int rdbSaveType(rio *rdb, unsigned char type) {//保存数据类型
    return rdbWriteRaw(rdb,&type,1);
}

/* Load a "type" in RDB format, that is a one byte unsigned integer.
 * This function is not only used to load object types, but also special
 * "types" like the end-of-file type, the EXPIRE type, and so forth. */
int rdbLoadType(rio *rdb) {//读取类型字段
    unsigned char type;
    if (rioRead(rdb,&type,1) == 0) return -1;
    return type;
}

time_t rdbLoadTime(rio *rdb) {//读取时间
    int32_t t32;
    if (rioRead(rdb,&t32,4) == 0) return -1;
    return (time_t)t32;
}

int rdbSaveMillisecondTime(rio *rdb, long long t) {
    int64_t t64 = (int64_t) t;
    return rdbWriteRaw(rdb,&t64,8);
}

long long rdbLoadMillisecondTime(rio *rdb) {
    int64_t t64;
    if (rioRead(rdb,&t64,8) == 0) return -1;
    return (long long)t64;
}

/* Saves an encoded length. The first two bits in the first byte are used to
 * hold the encoding type. See the REDIS_RDB_* definitions for more information
 * on the types of encoding. */
int rdbSaveLen(rio *rdb, uint32_t len) {
    unsigned char buf[2];
    size_t nwritten;

    if (len < (1<<6)) {//小于64
        /* Save a 6 bit len */
        buf[0] = (len&0xFF)|(REDIS_RDB_6BITLEN<<6);//第二位是0
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        nwritten = 1;
    } else if (len < (1<<14)) {
        /* Save a 14 bit len */
        buf[0] = ((len>>8)&0xFF)|(REDIS_RDB_14BITLEN<<6);//第二位是1
        buf[1] = len&0xFF;//保存后8位
        if (rdbWriteRaw(rdb,buf,2) == -1) return -1;
        nwritten = 2;
    } else {
        /* Save a 32 bit len */
        buf[0] = (REDIS_RDB_32BITLEN<<6);//前2位为10
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        len = htonl(len);//大端
        if (rdbWriteRaw(rdb,&len,4) == -1) return -1;
        nwritten = 1+4;
    }
    return nwritten;
}

/* Load an encoded length. The "isencoded" argument is set to 1 if the length
 * is not actually a length but an "encoding type". See the REDIS_RDB_ENC_*
 * definitions in rdb.h for more information. */
uint32_t rdbLoadLen(rio *rdb, int *isencoded) {//读取长度
    unsigned char buf[2];
    uint32_t len;
    int type;

    if (isencoded) *isencoded = 0;
    if (rioRead(rdb,buf,1) == 0) return REDIS_RDB_LENERR;
    type = (buf[0]&0xC0)>>6;//获取类型
    if (type == REDIS_RDB_ENCVAL) {//特殊类型
        /* Read a 6 bit encoding type. */
        if (isencoded) *isencoded = 1;
        return buf[0]&0x3F;
    } else if (type == REDIS_RDB_6BITLEN) {
        /* Read a 6 bit len. */
        return buf[0]&0x3F;//返回长度
    } else if (type == REDIS_RDB_14BITLEN) {
        /* Read a 14 bit len. */
        if (rioRead(rdb,buf+1,1) == 0) return REDIS_RDB_LENERR;
        return ((buf[0]&0x3F)<<8)|buf[1];//长度存储在后续的6+8位中
    } else {//后续4字节都是长度
        /* Read a 32 bit len. */
        if (rioRead(rdb,&len,4) == 0) return REDIS_RDB_LENERR;
        return ntohl(len);
    }
}

/* Encodes the "value" argument as integer when it fits in the supported ranges
 * for encoded types. If the function successfully encodes the integer, the
 * representation is stored in the buffer pointer to by "enc" and the string
 * length is returned. Otherwise 0 is returned. */
int rdbEncodeInteger(long long value, unsigned char *enc) {
    if (value >= -(1<<7) && value <= (1<<7)-1) {//-128~127
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT8;//第一个字节存储类型
        enc[1] = value&0xFF;
        return 2;
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {//16位
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    } else if (value >= -((long long)1<<31) && value <= ((long long)1<<31)-1) {//32位
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}

/* Loads an integer-encoded object with the specified encoding type "enctype".
 * If the "encode" argument is set the function may return an integer-encoded
 * string object, otherwise it always returns a raw string object. */
robj *rdbLoadIntegerObject(rio *rdb, int enctype, int encode) {//读取数字并返回redisobject
    unsigned char enc[4];
    long long val;

    if (enctype == REDIS_RDB_ENC_INT8) {//读取1字节
        if (rioRead(rdb,enc,1) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == REDIS_RDB_ENC_INT16) {//读取2字节数
        uint16_t v;
        if (rioRead(rdb,enc,2) == 0) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == REDIS_RDB_ENC_INT32) {//读取4字节长度整数
        uint32_t v;
        if (rioRead(rdb,enc,4) == 0) return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
        val = 0; /* anti-warning */
        redisPanic("Unknown RDB integer encoding type");
    }
    if (encode)//int编码
        return createStringObjectFromLongLong(val);
    else//raw编码
        return createObject(REDIS_STRING,sdsfromlonglong(val));
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space */
int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    long long value;
    char *endptr, buf[32];

    /* Check if it's possible to encode this value as a number */
    value = strtoll(s, &endptr, 10);
    if (endptr[0] != '\0') return 0;
    ll2string(buf,32,value);//longlong转string，每次转两位

    /* If the number converted back into a string is not identical
     * then it's not possible to encode the string as integer */
    if (strlen(buf) != len || memcmp(buf,s,len)) return 0;//转成数字失败

    return rdbEncodeInteger(value,enc);//编码int
}

int rdbSaveLzfStringObject(rio *rdb, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    unsigned char byte;
    int n, nwritten = 0;
    void *out;

    /* We require at least four bytes compression for this to be worth it */
    if (len <= 4) return 0;
    outlen = len-4;
    if ((out = zmalloc(outlen+1)) == NULL) return 0;
    comprlen = lzf_compress(s, len, out, outlen);//压缩
    if (comprlen == 0) {
        zfree(out);
        return 0;
    }
    /* Data compressed! Let's save it on disk */
    byte = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_LZF;//类型为压缩数据
    if ((n = rdbWriteRaw(rdb,&byte,1)) == -1) goto writeerr;//写类型
    nwritten += n;

    if ((n = rdbSaveLen(rdb,comprlen)) == -1) goto writeerr;//保存压缩数据长度
    nwritten += n;

    if ((n = rdbSaveLen(rdb,len)) == -1) goto writeerr;//保存压缩前数据长度
    nwritten += n;

    if ((n = rdbWriteRaw(rdb,out,comprlen)) == -1) goto writeerr;//写入压缩数据
    nwritten += n;

    zfree(out);//释放空间
    return nwritten;

writeerr:
    zfree(out);
    return -1;
}

robj *rdbLoadLzfStringObject(rio *rdb) {//读取数据
    unsigned int len, clen;
    unsigned char *c = NULL;
    sds val = NULL;

    if ((clen = rdbLoadLen(rdb,NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((len = rdbLoadLen(rdb,NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((c = zmalloc(clen)) == NULL) goto err;
    if ((val = sdsnewlen(NULL,len)) == NULL) goto err;
    if (rioRead(rdb,c,clen) == 0) goto err;
    if (lzf_decompress(c,clen,val,len) == 0) goto err;//解压数据
    zfree(c);
    return createObject(REDIS_STRING,val);
err:
    zfree(c);
    sdsfree(val);
    return NULL;
}

/* Save a string object as [len][data] on disk. If the object is a string
 * representation of an integer value we try to save it in a special form */
int rdbSaveRawString(rio *rdb, unsigned char *s, size_t len) {
    int enclen;
    int n, nwritten = 0;

    /* Try integer encoding */
    if (len <= 11) {//尝试数字编码
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {
            if (rdbWriteRaw(rdb,buf,enclen) == -1) return -1;
            return enclen;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even
     * aaaaaaaaaaaaaaaaaa so skip it */
    if (server.rdb_compression && len > 20) {//大于20字节，尝试lzf压缩
        n = rdbSaveLzfStringObject(rdb,s,len);
        if (n == -1) return -1;
        if (n > 0) return n;
        /* Return value of 0 means data can't be compressed, save the old way */
    }

    /* Store verbatim */
    if ((n = rdbSaveLen(rdb,len)) == -1) return -1;//保存长度
    nwritten += n;
    if (len > 0) {
        if (rdbWriteRaw(rdb,s,len) == -1) return -1;//保存数据
        nwritten += len;
    }
    return nwritten;
}

/* Save a long long value as either an encoded string or a string. */
int rdbSaveLongLongAsStringObject(rio *rdb, long long value) {
    unsigned char buf[32];
    int n, nwritten = 0;
    int enclen = rdbEncodeInteger(value,buf);//数字编码
    if (enclen > 0) {
        return rdbWriteRaw(rdb,buf,enclen);
    } else {
        /* Encode as string */
        enclen = ll2string((char*)buf,32,value);//转成字符串
        redisAssert(enclen < 32);
        if ((n = rdbSaveLen(rdb,enclen)) == -1) return -1;//保存长度
        nwritten += n;
        if ((n = rdbWriteRaw(rdb,buf,enclen)) == -1) return -1;//保存数据
        nwritten += n;
    }
    return nwritten;
}

/* Like rdbSaveStringObjectRaw() but handle encoded objects */
int rdbSaveStringObject(rio *rdb, robj *obj) {//保存对象
    /* Avoid to decode the object, then encode it again, if the
     * object is already integer encoded. */
    if (obj->encoding == REDIS_ENCODING_INT) {//已经是int编码
        return rdbSaveLongLongAsStringObject(rdb,(long)obj->ptr);
    } else {
        redisAssertWithInfo(NULL,obj,sdsEncodedObject(obj));
        return rdbSaveRawString(rdb,obj->ptr,sdslen(obj->ptr));//编码成数字或字符串进行保存
    }
}

robj *rdbGenericLoadStringObject(rio *rdb, int encode) {//读取数据
    int isencoded;
    uint32_t len;
    robj *o;

    len = rdbLoadLen(rdb,&isencoded);//获取长度，及是否编码
    if (isencoded) {
        switch(len) {
        case REDIS_RDB_ENC_INT8:
        case REDIS_RDB_ENC_INT16:
        case REDIS_RDB_ENC_INT32:
            return rdbLoadIntegerObject(rdb,len,encode);//读取数字
        case REDIS_RDB_ENC_LZF:
            return rdbLoadLzfStringObject(rdb);//读取压缩数据
        default:
            redisPanic("Unknown RDB encoding type");
        }
    }
    //未编码
    if (len == REDIS_RDB_LENERR) return NULL;
    o = encode ? createStringObject(NULL,len) :
                 createRawStringObject(NULL,len);
    if (len && rioRead(rdb,o->ptr,len) == 0) {//读取数据
        decrRefCount(o);
        return NULL;
    }
    return o;
}

robj *rdbLoadStringObject(rio *rdb) {//未编码数据
    return rdbGenericLoadStringObject(rdb,0);
}

robj *rdbLoadEncodedStringObject(rio *rdb) {//读取编码数据
    return rdbGenericLoadStringObject(rdb,1);
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifying the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 */
int rdbSaveDoubleValue(rio *rdb, double val) {//存储double类型
    unsigned char buf[128];
    int len;

    if (isnan(val)) {//非数字
        buf[0] = 253;
        len = 1;
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;
    } else {
#if (DBL_MANT_DIG >= 52) && (LLONG_MAX == 0x7fffffffffffffffLL)
        /* Check if the float is in a safe range to be casted into a
         * long long. We are assuming that long long is 64 bit here.
         * Also we are assuming that there are no implementations around where
         * double has precision < 52 bit.
         *
         * Under this assumptions we test if a double is inside an interval
         * where casting to long long is safe. Then using two castings we
         * make sure the decimal part is zero. If all this is true we use
         * integer printing function that is much faster. */
        double min = -4503599627370495; /* (2^52)-1 */
        double max = 4503599627370496; /* -(2^52) */
        if (val > min && val < max && val == ((double)((long long)val)))
            ll2string((char*)buf+1,sizeof(buf)-1,(long long)val);
        else
#endif
            snprintf((char*)buf+1,sizeof(buf)-1,"%.17g",val);
        buf[0] = strlen((char*)buf+1);//第一个字节存储长度
        len = buf[0]+1;
    }
    return rdbWriteRaw(rdb,buf,len);//写数据
}

/* For information about double serialization check rdbSaveDoubleValue() */
int rdbLoadDoubleValue(rio *rdb, double *val) {
    char buf[256];
    unsigned char len;

    if (rioRead(rdb,&len,1) == 0) return -1;//读取长度，以区分类型
    switch(len) {
    case 255: *val = R_NegInf; return 0;
    case 254: *val = R_PosInf; return 0;
    case 253: *val = R_Nan; return 0;
    default:
        if (rioRead(rdb,buf,len) == 0) return -1;//读取数据
        buf[len] = '\0';
        sscanf(buf, "%lg", val);
        return 0;
    }
}

/* Save the object type of object "o". */
int rdbSaveObjectType(rio *rdb, robj *o) {
    switch (o->type) {
    case REDIS_STRING://string
        return rdbSaveType(rdb,REDIS_RDB_TYPE_STRING);
    case REDIS_LIST:
        if (o->encoding == REDIS_ENCODING_ZIPLIST)//ziplist
            return rdbSaveType(rdb,REDIS_RDB_TYPE_LIST_ZIPLIST);
        else if (o->encoding == REDIS_ENCODING_LINKEDLIST)//linkedlist
            return rdbSaveType(rdb,REDIS_RDB_TYPE_LIST);
        else
            redisPanic("Unknown list encoding");
    case REDIS_SET:
        if (o->encoding == REDIS_ENCODING_INTSET)//intset
            return rdbSaveType(rdb,REDIS_RDB_TYPE_SET_INTSET);
        else if (o->encoding == REDIS_ENCODING_HT)//hashtable
            return rdbSaveType(rdb,REDIS_RDB_TYPE_SET);
        else
            redisPanic("Unknown set encoding");
    case REDIS_ZSET:
        if (o->encoding == REDIS_ENCODING_ZIPLIST)//ziplist
            return rdbSaveType(rdb,REDIS_RDB_TYPE_ZSET_ZIPLIST);
        else if (o->encoding == REDIS_ENCODING_SKIPLIST)//skiplist
            return rdbSaveType(rdb,REDIS_RDB_TYPE_ZSET);
        else
            redisPanic("Unknown sorted set encoding");
    case REDIS_HASH:
        if (o->encoding == REDIS_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,REDIS_RDB_TYPE_HASH_ZIPLIST);
        else if (o->encoding == REDIS_ENCODING_HT)
            return rdbSaveType(rdb,REDIS_RDB_TYPE_HASH);
        else
            redisPanic("Unknown hash encoding");
    default:
        redisPanic("Unknown object type");
    }
    return -1; /* avoid warning */
}

/* Use rdbLoadType() to load a TYPE in RDB format, but returns -1 if the
 * type is not specifically a valid Object Type. */
int rdbLoadObjectType(rio *rdb) {
    int type;
    if ((type = rdbLoadType(rdb)) == -1) return -1;//加载数据类型
    if (!rdbIsObjectType(type)) return -1;//
    return type;
}

/* Save a Redis object. Returns -1 on error, number of bytes written on success. */
int rdbSaveObject(rio *rdb, robj *o) {//保存对象
    int n, nwritten = 0;

    if (o->type == REDIS_STRING) {
        /* Save a string value */
        if ((n = rdbSaveStringObject(rdb,o)) == -1) return -1;//保存string，三种编码，int，raw，sds
        nwritten += n;
    } else if (o->type == REDIS_LIST) {//链表
        /* Save a list value */
        if (o->encoding == REDIS_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);//获取ziplist长度

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;//ziplist是连续的，可以直接保存
            nwritten += n;
        } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
            list *list = o->ptr;
            listIter li;
            listNode *ln;

            if ((n = rdbSaveLen(rdb,listLength(list))) == -1) return -1;//保存链表长度
            nwritten += n;

            listRewind(list,&li);//创建一个指向链表的指针
            while((ln = listNext(&li))) {//遍历链表，存储每个节点
                robj *eleobj = listNodeValue(ln);
                if ((n = rdbSaveStringObject(rdb,eleobj)) == -1) return -1;
                nwritten += n;
            }
        } else {
            redisPanic("Unknown list encoding");
        }
    } else if (o->type == REDIS_SET) {//集合
        /* Save a set value */
        if (o->encoding == REDIS_ENCODING_HT) {
            dict *set = o->ptr;
            dictIterator *di = dictGetIterator(set);//初始化迭代器
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,dictSize(set))) == -1) return -1;
            nwritten += n;

            while((de = dictNext(di)) != NULL) {//遍历字典
                robj *eleobj = dictGetKey(de);//获取key
                if ((n = rdbSaveStringObject(rdb,eleobj)) == -1) return -1;//保存key，set只有key
                nwritten += n;
            }
            dictReleaseIterator(di);//释放迭代器，检查指纹
        } else if (o->encoding == REDIS_ENCODING_INTSET) {//intset
            size_t l = intsetBlobLen((intset*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;//，连续数据，直接保存
            nwritten += n;
        } else {
            redisPanic("Unknown set encoding");
        }
    } else if (o->type == REDIS_ZSET) {
        /* Save a sorted set value */
        if (o->encoding == REDIS_ENCODING_ZIPLIST) {//ziplist
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);//获取压缩表大小

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;//压缩表，连续数据，直接保存
            nwritten += n;
        } else if (o->encoding == REDIS_ENCODING_SKIPLIST) {//跳跃表
            zset *zs = o->ptr;
            dictIterator *di = dictGetIterator(zs->dict);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,dictSize(zs->dict))) == -1) return -1;//保存字典大小
            nwritten += n;

            while((de = dictNext(di)) != NULL) {//遍历字典
                robj *eleobj = dictGetKey(de);//数据
                double *score = dictGetVal(de);//分数

                if ((n = rdbSaveStringObject(rdb,eleobj)) == -1) return -1;
                nwritten += n;
                if ((n = rdbSaveDoubleValue(rdb,*score)) == -1) return -1;
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else {
            redisPanic("Unknown sorted set encoding");
        }
    } else if (o->type == REDIS_HASH) {//hashtable
        /* Save a hash value */
        if (o->encoding == REDIS_ENCODING_ZIPLIST) {//压缩表存储
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);//保存压缩表

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;

        } else if (o->encoding == REDIS_ENCODING_HT) {//hashtable
            dictIterator *di = dictGetIterator(o->ptr);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,dictSize((dict*)o->ptr))) == -1) return -1;
            nwritten += n;

            while((de = dictNext(di)) != NULL) {//遍历链表，保存键值
                robj *key = dictGetKey(de);
                robj *val = dictGetVal(de);

                if ((n = rdbSaveStringObject(rdb,key)) == -1) return -1;
                nwritten += n;
                if ((n = rdbSaveStringObject(rdb,val)) == -1) return -1;
                nwritten += n;
            }
            dictReleaseIterator(di);

        } else {
            redisPanic("Unknown hash encoding");
        }

    } else {
        redisPanic("Unknown object type");
    }
    return nwritten;
}

/* Return the length the object will have on disk if saved with
 * the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future
 * we could switch to a faster solution. */
off_t rdbSavedObjectLen(robj *o) {
    int len = rdbSaveObject(NULL,o);
    redisAssertWithInfo(NULL,o,len != -1);
    return len;
}

/* Save a key-value pair, with expire time, type, key, value.
 * On error -1 is returned.
 * On success if the key was actually saved 1 is returned, otherwise 0
 * is returned (the key was already expired). */
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val,
                        long long expiretime, long long now)
{
    /* Save the expire time */
    if (expiretime != -1) {
        /* If this key is already expired skip it */
        if (expiretime < now) return 0;//已过期
        if (rdbSaveType(rdb,REDIS_RDB_OPCODE_EXPIRETIME_MS) == -1) return -1;//保存过期时间单位
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) return -1;//保存过期时间
    }

    /* Save type, key, value *///保存类型键值
    if (rdbSaveObjectType(rdb,val) == -1) return -1;
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    if (rdbSaveObject(rdb,val) == -1) return -1;
    return 1;
}

/* Produces a dump of the database in RDB format sending it to the specified
 * Redis I/O channel. On success REDIS_OK is returned, otherwise REDIS_ERR
 * is returned and part of the output, or all the output, can be
 * missing because of I/O errors.
 *
 * When the function returns REDIS_ERR and if 'error' is not NULL, the
 * integer pointed by 'error' is set to the value of errno just after the I/O
 * error. */
int rdbSaveRio(rio *rdb, int *error) {
    dictIterator *di = NULL;
    dictEntry *de;
    char magic[10];
    int j;
    long long now = mstime();
    uint64_t cksum;

    if (server.rdb_checksum)
        rdb->update_cksum = rioGenericUpdateChecksum;
    snprintf(magic,sizeof(magic),"REDIS%04d",REDIS_RDB_VERSION);
    if (rdbWriteRaw(rdb,magic,9) == -1) goto werr;

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;//获取第j个数据库地址
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetSafeIterator(d);
        if (!di) return REDIS_ERR;

        /* Write the SELECT DB opcode */
        if (rdbSaveType(rdb,REDIS_RDB_OPCODE_SELECTDB) == -1) goto werr;
        if (rdbSaveLen(rdb,j) == -1) goto werr;

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {//保存数据库键值
            sds keystr = dictGetKey(de);
            robj key, *o = dictGetVal(de);
            long long expire;

            initStaticStringObject(key,keystr);
            expire = getExpire(db,&key);//获取过期时间
            if (rdbSaveKeyValuePair(rdb,&key,o,expire,now) == -1) goto werr;//存储键与过期时间键值对
        }
        dictReleaseIterator(di);
    }
    di = NULL; /* So that we don't release it again on error. */

    /* EOF opcode */
    if (rdbSaveType(rdb,REDIS_RDB_OPCODE_EOF) == -1) goto werr;//写入结束符

    /* CRC64 checksum. It will be zero if checksum computation is disabled, the
     * loading code skips the check in this case. */
    cksum = rdb->cksum;
    memrev64ifbe(&cksum);
    if (rioWrite(rdb,&cksum,8) == 0) goto werr;//写入校验和
    return REDIS_OK;

werr:
    if (error) *error = errno;
    if (di) dictReleaseIterator(di);
    return REDIS_ERR;
}

/* This is just a wrapper to rdbSaveRio() that additionally adds a prefix
 * and a suffix to the generated RDB dump. The prefix is:
 *
 * $EOF:<40 bytes unguessable hex string>\r\n
 *
 * While the suffix is the 40 bytes hex string we announced in the prefix.
 * This way processes receiving the payload can understand when it ends
 * without doing any processing of the content. */
int rdbSaveRioWithEOFMark(rio *rdb, int *error) {//增加dump文件前缀后缀
    char eofmark[REDIS_EOF_MARK_SIZE];

    getRandomHexChars(eofmark,REDIS_EOF_MARK_SIZE);
    if (error) *error = 0;
    if (rioWrite(rdb,"$EOF:",5) == 0) goto werr;
    if (rioWrite(rdb,eofmark,REDIS_EOF_MARK_SIZE) == 0) goto werr;
    if (rioWrite(rdb,"\r\n",2) == 0) goto werr;
    if (rdbSaveRio(rdb,error) == REDIS_ERR) goto werr;
    if (rioWrite(rdb,eofmark,REDIS_EOF_MARK_SIZE) == 0) goto werr;
    return REDIS_OK;

werr: /* Write error. */
    /* Set 'error' only if not already set by rdbSaveRio() call. */
    if (error && *error == 0) *error = errno;
    return REDIS_ERR;
}

/* Save the DB on disk. Return REDIS_ERR on error, REDIS_OK on success. */
int rdbSave(char *filename) {
    char tmpfile[256];
    FILE *fp;
    rio rdb;
    int error;

    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    fp = fopen(tmpfile,"w");//创建文件
    if (!fp) {
        redisLog(REDIS_WARNING, "Failed opening .rdb for saving: %s",
            strerror(errno));
        return REDIS_ERR;
    }

    rioInitWithFile(&rdb,fp);
    if (rdbSaveRio(&rdb,&error) == REDIS_ERR) {//保存
        errno = error;
        goto werr;
    }

    /* Make sure data will not remain on the OS's output buffers */
    if (fflush(fp) == EOF) goto werr;
    if (fsync(fileno(fp)) == -1) goto werr;
    if (fclose(fp) == EOF) goto werr;

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {//修改名字
        redisLog(REDIS_WARNING,"Error moving temp DB file on the final destination: %s", strerror(errno));
        unlink(tmpfile);//失败则删除
        return REDIS_ERR;
    }
    redisLog(REDIS_NOTICE,"DB saved on disk");
    server.dirty = 0;
    server.lastsave = time(NULL);
    server.lastbgsave_status = REDIS_OK;
    return REDIS_OK;

werr:
    redisLog(REDIS_WARNING,"Write error saving DB on disk: %s", strerror(errno));
    fclose(fp);
    unlink(tmpfile);
    return REDIS_ERR;
}

int rdbSaveBackground(char *filename) {
    pid_t childpid;
    long long start;

    if (server.rdb_child_pid != -1) return REDIS_ERR;

    server.dirty_before_bgsave = server.dirty;
    server.lastbgsave_try = time(NULL);

    start = ustime();
    if ((childpid = fork()) == 0) {//fork之后，父进程的数据被拷贝到子进程
        int retval;

        /* Child */
        closeListeningSockets(0);//关闭套接字
        redisSetProcTitle("redis-rdb-bgsave");
        retval = rdbSave(filename);//保存数据库
        if (retval == REDIS_OK) {
            size_t private_dirty = zmalloc_get_private_dirty();//查看进程已改私有页面

            if (private_dirty) {
                redisLog(REDIS_NOTICE,
                    "RDB: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }
        }
        exitFromChild((retval == REDIS_OK) ? 0 : 1);//退出进程
    } else {
        /* Parent */
        server.stat_fork_time = ustime()-start;//fork花的时间
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
        if (childpid == -1) {//子进程创建失败
            server.lastbgsave_status = REDIS_ERR;
            redisLog(REDIS_WARNING,"Can't save in background: fork: %s",
                strerror(errno));
            return REDIS_ERR;
        }
        redisLog(REDIS_NOTICE,"Background saving started by pid %d",childpid);
        server.rdb_save_time_start = time(NULL);
        server.rdb_child_pid = childpid;
        server.rdb_child_type = REDIS_RDB_CHILD_TYPE_DISK;
        updateDictResizePolicy();//备份期间避免hashtable的resize
        return REDIS_OK;
    }
    return REDIS_OK; /* unreached */
}

void rdbRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,sizeof(tmpfile),"temp-%d.rdb", (int) childpid);
    unlink(tmpfile);//没有链接之后就删除文件
}

/* Load a Redis object of the specified type from the specified file.
 * On success a newly allocated object is returned, otherwise NULL. */
robj *rdbLoadObject(int rdbtype, rio *rdb) {//读取对象
    robj *o, *ele, *dec;
    size_t len;
    unsigned int i;

    if (rdbtype == REDIS_RDB_TYPE_STRING) {//读取string
        /* Read string value */
        if ((o = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
        o = tryObjectEncoding(o);
    } else if (rdbtype == REDIS_RDB_TYPE_LIST) {//读取list
        /* Read list value */
        if ((len = rdbLoadLen(rdb,NULL)) == REDIS_RDB_LENERR) return NULL;

        /* Use a real list when there are too many entries */
        if (len > server.list_max_ziplist_entries) {
            o = createListObject();
        } else {
            o = createZiplistObject();
        }

        /* Load every single element of the list */
        while(len--) {
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;

            /* If we are using a ziplist and the value is too big, convert
             * the object to a real list. */
            if (o->encoding == REDIS_ENCODING_ZIPLIST &&
                sdsEncodedObject(ele) &&
                sdslen(ele->ptr) > server.list_max_ziplist_value)
                    listTypeConvert(o,REDIS_ENCODING_LINKEDLIST);//转成双端链表

            if (o->encoding == REDIS_ENCODING_ZIPLIST) {//压缩链表
                dec = getDecodedObject(ele);
                o->ptr = ziplistPush(o->ptr,dec->ptr,sdslen(dec->ptr),REDIS_TAIL);
                decrRefCount(dec);
                decrRefCount(ele);
            } else {
                ele = tryObjectEncoding(ele);
                listAddNodeTail(o->ptr,ele);
            }
        }
    } else if (rdbtype == REDIS_RDB_TYPE_SET) {
        /* Read list/set value */
        if ((len = rdbLoadLen(rdb,NULL)) == REDIS_RDB_LENERR) return NULL;

        /* Use a regular set when there are too many entries. */
        if (len > server.set_max_intset_entries) {
            o = createSetObject();
            /* It's faster to expand the dict to the right size asap in order
             * to avoid rehashing */
            if (len > DICT_HT_INITIAL_SIZE)
                dictExpand(o->ptr,len);
        } else {
            o = createIntsetObject();
        }

        /* Load every single element of the list/set */
        for (i = 0; i < len; i++) {
            long long llval;
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
            ele = tryObjectEncoding(ele);

            if (o->encoding == REDIS_ENCODING_INTSET) {
                /* Fetch integer value from element */
                if (isObjectRepresentableAsLongLong(ele,&llval) == REDIS_OK) {
                    o->ptr = intsetAdd(o->ptr,llval,NULL);
                } else {
                    setTypeConvert(o,REDIS_ENCODING_HT);
                    dictExpand(o->ptr,len);
                }
            }

            /* This will also be called when the set was just converted
             * to a regular hash table encoded set */
            if (o->encoding == REDIS_ENCODING_HT) {
                dictAdd((dict*)o->ptr,ele,NULL);
            } else {
                decrRefCount(ele);
            }
        }
    } else if (rdbtype == REDIS_RDB_TYPE_ZSET) {
        /* Read list/set value */
        size_t zsetlen;
        size_t maxelelen = 0;
        zset *zs;

        if ((zsetlen = rdbLoadLen(rdb,NULL)) == REDIS_RDB_LENERR) return NULL;
        o = createZsetObject();
        zs = o->ptr;

        /* Load every single element of the list/set */
        while(zsetlen--) {
            robj *ele;
            double score;
            zskiplistNode *znode;

            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
            ele = tryObjectEncoding(ele);
            if (rdbLoadDoubleValue(rdb,&score) == -1) return NULL;

            /* Don't care about integer-encoded strings. */
            if (sdsEncodedObject(ele) && sdslen(ele->ptr) > maxelelen)
                maxelelen = sdslen(ele->ptr);

            znode = zslInsert(zs->zsl,score,ele);
            dictAdd(zs->dict,ele,&znode->score);
            incrRefCount(ele); /* added to skiplist */
        }

        /* Convert *after* loading, since sorted sets are not stored ordered. */
        if (zsetLength(o) <= server.zset_max_ziplist_entries &&
            maxelelen <= server.zset_max_ziplist_value)
                zsetConvert(o,REDIS_ENCODING_ZIPLIST);
    } else if (rdbtype == REDIS_RDB_TYPE_HASH) {
        size_t len;
        int ret;

        len = rdbLoadLen(rdb, NULL);
        if (len == REDIS_RDB_LENERR) return NULL;

        o = createHashObject();

        /* Too many entries? Use a hash table. */
        if (len > server.hash_max_ziplist_entries)
            hashTypeConvert(o, REDIS_ENCODING_HT);

        /* Load every field and value into the ziplist */
        while (o->encoding == REDIS_ENCODING_ZIPLIST && len > 0) {
            robj *field, *value;

            len--;
            /* Load raw strings */
            field = rdbLoadStringObject(rdb);
            if (field == NULL) return NULL;
            redisAssert(sdsEncodedObject(field));
            value = rdbLoadStringObject(rdb);
            if (value == NULL) return NULL;
            redisAssert(sdsEncodedObject(value));

            /* Add pair to ziplist */
            o->ptr = ziplistPush(o->ptr, field->ptr, sdslen(field->ptr), ZIPLIST_TAIL);
            o->ptr = ziplistPush(o->ptr, value->ptr, sdslen(value->ptr), ZIPLIST_TAIL);
            /* Convert to hash table if size threshold is exceeded */
            if (sdslen(field->ptr) > server.hash_max_ziplist_value ||
                sdslen(value->ptr) > server.hash_max_ziplist_value)
            {
                decrRefCount(field);
                decrRefCount(value);
                hashTypeConvert(o, REDIS_ENCODING_HT);
                break;
            }
            decrRefCount(field);
            decrRefCount(value);
        }

        /* Load remaining fields and values into the hash table */
        while (o->encoding == REDIS_ENCODING_HT && len > 0) {
            robj *field, *value;

            len--;
            /* Load encoded strings */
            field = rdbLoadEncodedStringObject(rdb);
            if (field == NULL) return NULL;
            value = rdbLoadEncodedStringObject(rdb);
            if (value == NULL) return NULL;

            field = tryObjectEncoding(field);
            value = tryObjectEncoding(value);

            /* Add pair to hash table */
            ret = dictAdd((dict*)o->ptr, field, value);
            redisAssert(ret == DICT_OK);
        }

        /* All pairs should be read by now */
        redisAssert(len == 0);

    } else if (rdbtype == REDIS_RDB_TYPE_HASH_ZIPMAP  ||
               rdbtype == REDIS_RDB_TYPE_LIST_ZIPLIST ||
               rdbtype == REDIS_RDB_TYPE_SET_INTSET   ||
               rdbtype == REDIS_RDB_TYPE_ZSET_ZIPLIST ||
               rdbtype == REDIS_RDB_TYPE_HASH_ZIPLIST)
    {
        robj *aux = rdbLoadStringObject(rdb);

        if (aux == NULL) return NULL;
        o = createObject(REDIS_STRING,NULL); /* string is just placeholder */
        o->ptr = zmalloc(sdslen(aux->ptr));
        memcpy(o->ptr,aux->ptr,sdslen(aux->ptr));
        decrRefCount(aux);

        /* Fix the object encoding, and make sure to convert the encoded
         * data type into the base type if accordingly to the current
         * configuration there are too many elements in the encoded data
         * type. Note that we only check the length and not max element
         * size as this is an O(N) scan. Eventually everything will get
         * converted. */
        switch(rdbtype) {
            case REDIS_RDB_TYPE_HASH_ZIPMAP:
                /* Convert to ziplist encoded hash. This must be deprecated
                 * when loading dumps created by Redis 2.4 gets deprecated. */
                {
                    unsigned char *zl = ziplistNew();
                    unsigned char *zi = zipmapRewind(o->ptr);
                    unsigned char *fstr, *vstr;
                    unsigned int flen, vlen;
                    unsigned int maxlen = 0;

                    while ((zi = zipmapNext(zi, &fstr, &flen, &vstr, &vlen)) != NULL) {
                        if (flen > maxlen) maxlen = flen;
                        if (vlen > maxlen) maxlen = vlen;
                        zl = ziplistPush(zl, fstr, flen, ZIPLIST_TAIL);
                        zl = ziplistPush(zl, vstr, vlen, ZIPLIST_TAIL);
                    }

                    zfree(o->ptr);
                    o->ptr = zl;
                    o->type = REDIS_HASH;
                    o->encoding = REDIS_ENCODING_ZIPLIST;

                    if (hashTypeLength(o) > server.hash_max_ziplist_entries ||
                        maxlen > server.hash_max_ziplist_value)
                    {
                        hashTypeConvert(o, REDIS_ENCODING_HT);
                    }
                }
                break;
            case REDIS_RDB_TYPE_LIST_ZIPLIST:
                o->type = REDIS_LIST;
                o->encoding = REDIS_ENCODING_ZIPLIST;
                if (ziplistLen(o->ptr) > server.list_max_ziplist_entries)
                    listTypeConvert(o,REDIS_ENCODING_LINKEDLIST);
                break;
            case REDIS_RDB_TYPE_SET_INTSET:
                o->type = REDIS_SET;
                o->encoding = REDIS_ENCODING_INTSET;
                if (intsetLen(o->ptr) > server.set_max_intset_entries)
                    setTypeConvert(o,REDIS_ENCODING_HT);
                break;
            case REDIS_RDB_TYPE_ZSET_ZIPLIST:
                o->type = REDIS_ZSET;
                o->encoding = REDIS_ENCODING_ZIPLIST;
                if (zsetLength(o) > server.zset_max_ziplist_entries)
                    zsetConvert(o,REDIS_ENCODING_SKIPLIST);
                break;
            case REDIS_RDB_TYPE_HASH_ZIPLIST:
                o->type = REDIS_HASH;
                o->encoding = REDIS_ENCODING_ZIPLIST;
                if (hashTypeLength(o) > server.hash_max_ziplist_entries)
                    hashTypeConvert(o, REDIS_ENCODING_HT);
                break;
            default:
                redisPanic("Unknown encoding");
                break;
        }
    } else {
        redisPanic("Unknown object type");
    }
    return o;
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats. */
void startLoading(FILE *fp) {//初始化load状态
    struct stat sb;

    /* Load the DB */
    server.loading = 1;
    server.loading_start_time = time(NULL);
    server.loading_loaded_bytes = 0;
    if (fstat(fileno(fp), &sb) == -1) {
        server.loading_total_bytes = 0;
    } else {
        server.loading_total_bytes = sb.st_size;
    }
}

/* Refresh the loading progress info */
void loadingProgress(off_t pos) {
    server.loading_loaded_bytes = pos;
    if (server.stat_peak_memory < zmalloc_used_memory())
        server.stat_peak_memory = zmalloc_used_memory();
}

/* Loading finished */
void stopLoading(void) {
    server.loading = 0;
}

/* Track loading progress in order to serve client's from time to time
   and if needed calculate rdb checksum  */
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len) {
    if (server.rdb_checksum)
        rioGenericUpdateChecksum(r, buf, len);//更新校验和
    if (server.loading_process_events_interval_bytes &&
        (r->processed_bytes + len)/server.loading_process_events_interval_bytes > r->processed_bytes/server.loading_process_events_interval_bytes)
    {
        /* The DB can take some non trivial amount of time to load. Update
         * our cached time since it is used to create and update the last
         * interaction time with clients and for other important things. */
        updateCachedTime();
        if (server.masterhost && server.repl_state == REDIS_REPL_TRANSFER)
            replicationSendNewlineToMaster();
        loadingProgress(r->processed_bytes);
        processEventsWhileBlocked();
    }
}

int rdbLoad(char *filename) {//rdb文件加载
    uint32_t dbid;
    int type, rdbver;
    redisDb *db = server.db+0;
    char buf[1024];
    long long expiretime, now = mstime();
    FILE *fp;
    rio rdb;

    if ((fp = fopen(filename,"r")) == NULL) return REDIS_ERR;

    rioInitWithFile(&rdb,fp);
    rdb.update_cksum = rdbLoadProgressCallback;
    rdb.max_processing_chunk = server.loading_process_events_interval_bytes;
    if (rioRead(&rdb,buf,9) == 0) goto eoferr;//读取前9个字节
    buf[9] = '\0';
    if (memcmp(buf,"REDIS",5) != 0) {
        fclose(fp);
        redisLog(REDIS_WARNING,"Wrong signature trying to load DB from file");
        errno = EINVAL;
        return REDIS_ERR;
    }
    rdbver = atoi(buf+5);
    if (rdbver < 1 || rdbver > REDIS_RDB_VERSION) {
        fclose(fp);
        redisLog(REDIS_WARNING,"Can't handle RDB format version %d",rdbver);
        errno = EINVAL;
        return REDIS_ERR;
    }

    startLoading(fp);
    while(1) {
        robj *key, *val;
        expiretime = -1;

        /* Read type. */
        if ((type = rdbLoadType(&rdb)) == -1) goto eoferr;
        if (type == REDIS_RDB_OPCODE_EXPIRETIME) {//加载过期时间
            if ((expiretime = rdbLoadTime(&rdb)) == -1) goto eoferr;
            /* We read the time so we need to read the object type again. */
            if ((type = rdbLoadType(&rdb)) == -1) goto eoferr;//加载类型
            /* the EXPIRETIME opcode specifies time in seconds, so convert
             * into milliseconds. */
            expiretime *= 1000;
        } else if (type == REDIS_RDB_OPCODE_EXPIRETIME_MS) {//毫秒为单位的过期时间
            /* Milliseconds precision expire times introduced with RDB
             * version 3. */
            if ((expiretime = rdbLoadMillisecondTime(&rdb)) == -1) goto eoferr;
            /* We read the time so we need to read the object type again. */
            if ((type = rdbLoadType(&rdb)) == -1) goto eoferr;
        }

        if (type == REDIS_RDB_OPCODE_EOF)
            break;

        /* Handle SELECT DB opcode as a special case */
        if (type == REDIS_RDB_OPCODE_SELECTDB) {//数据库ID
            if ((dbid = rdbLoadLen(&rdb,NULL)) == REDIS_RDB_LENERR)
                goto eoferr;
            if (dbid >= (unsigned)server.dbnum) {
                redisLog(REDIS_WARNING,"FATAL: Data file was created with a Redis server configured to handle more than %d databases. Exiting\n", server.dbnum);
                exit(1);
            }
            db = server.db+dbid;
            continue;
        }
        /* Read key *///加载键值
        if ((key = rdbLoadStringObject(&rdb)) == NULL) goto eoferr;
        /* Read value */
        if ((val = rdbLoadObject(type,&rdb)) == NULL) goto eoferr;
        /* Check if the key already expired. This function is used when loading
         * an RDB file from disk, either at startup, or when an RDB was
         * received from the master. In the latter case, the master is
         * responsible for key expiry. If we would expire keys here, the
         * snapshot taken by the master may not be reflected on the slave. */
        if (server.masterhost == NULL && expiretime != -1 && expiretime < now) {
            decrRefCount(key);
            decrRefCount(val);
            continue;
        }
        /* Add the new object in the hash table */
        dbAdd(db,key,val);//将键值添加到数据库

        /* Set the expire time if needed */
        if (expiretime != -1) setExpire(db,key,expiretime);//设置过期时间

        decrRefCount(key);
    }
    /* Verify the checksum if RDB version is >= 5 */
    if (rdbver >= 5 && server.rdb_checksum) {
        uint64_t cksum, expected = rdb.cksum;

        if (rioRead(&rdb,&cksum,8) == 0) goto eoferr;
        memrev64ifbe(&cksum);
        if (cksum == 0) {
            redisLog(REDIS_WARNING,"RDB file was saved with checksum disabled: no check performed.");
        } else if (cksum != expected) {
            redisLog(REDIS_WARNING,"Wrong RDB checksum. Aborting now.");
            exit(1);
        }
    }

    fclose(fp);
    stopLoading();
    return REDIS_OK;

eoferr: /* unexpected end of file is handled here with a fatal exit */
    redisLog(REDIS_WARNING,"Short read or OOM loading DB. Unrecoverable error, aborting now.");
    exit(1);
    return REDIS_ERR; /* Just to avoid warning */
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of actual BGSAVEs. */
void backgroundSaveDoneHandlerDisk(int exitcode, int bysignal) {//处理备份结束
    if (!bysignal && exitcode == 0) {//成功
        redisLog(REDIS_NOTICE,
            "Background saving terminated with success");
        server.dirty = server.dirty - server.dirty_before_bgsave;
        server.lastsave = time(NULL);
        server.lastbgsave_status = REDIS_OK;
    } else if (!bysignal && exitcode != 0) {//失败
        redisLog(REDIS_WARNING, "Background saving error");
        server.lastbgsave_status = REDIS_ERR;
    } else {//警告
        mstime_t latency;

        redisLog(REDIS_WARNING,
            "Background saving terminated by signal %d", bysignal);
        latencyStartMonitor(latency);
        rdbRemoveTempFile(server.rdb_child_pid);
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("rdb-unlink-temp-file",latency);
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * tirggering an error conditon. */
        if (bysignal != SIGUSR1)
            server.lastbgsave_status = REDIS_ERR;
    }
    server.rdb_child_pid = -1;
    server.rdb_child_type = REDIS_RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_last = time(NULL)-server.rdb_save_time_start;
    server.rdb_save_time_start = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) *///更新从服务器
    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? REDIS_OK : REDIS_ERR, REDIS_RDB_CHILD_TYPE_DISK);
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of RDB -> Salves socket transfers for
 * diskless replication. */
void backgroundSaveDoneHandlerSocket(int exitcode, int bysignal) {
    uint64_t *ok_slaves;

    if (!bysignal && exitcode == 0) {
        redisLog(REDIS_NOTICE,
            "Background RDB transfer terminated with success");
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING, "Background transfer error");
    } else {
        redisLog(REDIS_WARNING,
            "Background transfer terminated by signal %d", bysignal);
    }
    server.rdb_child_pid = -1;
    server.rdb_child_type = REDIS_RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_start = -1;

    /* If the child returns an OK exit code, read the set of slave client
     * IDs and the associated status code. We'll terminate all the slaves
     * in error state.
     *
     * If the process returned an error, consider the list of slaves that
     * can continue to be emtpy, so that it's just a special case of the
     * normal code path. */
    ok_slaves = zmalloc(sizeof(uint64_t)); /* Make space for the count. */
    ok_slaves[0] = 0;
    if (!bysignal && exitcode == 0) {
        int readlen = sizeof(uint64_t);

        if (read(server.rdb_pipe_read_result_from_child, ok_slaves, readlen) ==
                 readlen)
        {
            readlen = ok_slaves[0]*sizeof(uint64_t)*2;

            /* Make space for enough elements as specified by the first
             * uint64_t element in the array. */
            ok_slaves = zrealloc(ok_slaves,sizeof(uint64_t)+readlen);
            if (readlen &&
                read(server.rdb_pipe_read_result_from_child, ok_slaves+1,
                     readlen) != readlen)
            {
                ok_slaves[0] = 0;
            }
        }
    }

    close(server.rdb_pipe_read_result_from_child);
    close(server.rdb_pipe_write_result_to_parent);

    /* We can continue the replication process with all the slaves that
     * correctly received the full payload. Others are terminated. */
    listNode *ln;
    listIter li;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) {
            uint64_t j;
            int errorcode = 0;

            /* Search for the slave ID in the reply. In order for a slave to
             * continue the replication process, we need to find it in the list,
             * and it must have an error code set to 0 (which means success). */
            for (j = 0; j < ok_slaves[0]; j++) {
                if (slave->id == ok_slaves[2*j+1]) {
                    errorcode = ok_slaves[2*j+2];
                    break; /* Found in slaves list. */
                }
            }
            if (j == ok_slaves[0] || errorcode != 0) {
                redisLog(REDIS_WARNING,
                "Closing slave %s: child->slave RDB transfer failed: %s",
                    replicationGetSlaveName(slave),
                    (errorcode == 0) ? "RDB transfer child aborted"
                                     : strerror(errorcode));
                freeClient(slave);
            } else {
                redisLog(REDIS_WARNING,
                "Slave %s correctly received the streamed RDB file.",
                    replicationGetSlaveName(slave));
                /* Restore the socket as non-blocking. */
                anetNonBlock(NULL,slave->fd);
                anetSendTimeout(NULL,slave->fd,0);
            }
        }
    }
    zfree(ok_slaves);

    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? REDIS_OK : REDIS_ERR, REDIS_RDB_CHILD_TYPE_SOCKET);
}

/* When a background RDB saving/transfer terminates, call the right handler. */
void backgroundSaveDoneHandler(int exitcode, int bysignal) {
    switch(server.rdb_child_type) {
    case REDIS_RDB_CHILD_TYPE_DISK:
        backgroundSaveDoneHandlerDisk(exitcode,bysignal);
        break;
    case REDIS_RDB_CHILD_TYPE_SOCKET:
        backgroundSaveDoneHandlerSocket(exitcode,bysignal);
        break;
    default:
        redisPanic("Unknown RDB child type.");
        break;
    }
}

/* Spawn an RDB child that writes the RDB to the sockets of the slaves
 * that are currently in REDIS_REPL_WAIT_BGSAVE_START state. */
int rdbSaveToSlavesSockets(void) {
    int *fds;
    uint64_t *clientids;
    int numfds;
    listNode *ln;
    listIter li;
    pid_t childpid;
    long long start;
    int pipefds[2];

    if (server.rdb_child_pid != -1) return REDIS_ERR;

    /* Before to fork, create a pipe that will be used in order to
     * send back to the parent the IDs of the slaves that successfully
     * received all the writes. */
    if (pipe(pipefds) == -1) return REDIS_ERR;//创建管道
    server.rdb_pipe_read_result_from_child = pipefds[0];
    server.rdb_pipe_write_result_to_parent = pipefds[1];

    /* Collect the file descriptors of the slaves we want to transfer
     * the RDB to, which are i WAIT_BGSAVE_START state. */
    fds = zmalloc(sizeof(int)*listLength(server.slaves));
    /* We also allocate an array of corresponding client IDs. This will
     * be useful for the child process in order to build the report
     * (sent via unix pipe) that will be sent to the parent. */
    clientids = zmalloc(sizeof(uint64_t)*listLength(server.slaves));
    numfds = 0;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) {
            clientids[numfds] = slave->id;
            fds[numfds++] = slave->fd;
            replicationSetupSlaveForFullResync(slave,getPsyncInitialOffset());
            /* Put the socket in non-blocking mode to simplify RDB transfer.
             * We'll restore it when the children returns (since duped socket
             * will share the O_NONBLOCK attribute with the parent). */
            anetBlock(NULL,slave->fd);//设置成非阻塞
            anetSendTimeout(NULL,slave->fd,server.repl_timeout*1000);
        }
    }

    /* Create the child process. */
    start = ustime();
    if ((childpid = fork()) == 0) {//创建子进程
        /* Child */
        int retval;
        rio slave_sockets;

        rioInitWithFdset(&slave_sockets,fds,numfds);
        zfree(fds);

        closeListeningSockets(0);
        redisSetProcTitle("redis-rdb-to-slaves");

        retval = rdbSaveRioWithEOFMark(&slave_sockets,NULL);//发送数据给slave
        if (retval == REDIS_OK && rioFlush(&slave_sockets) == 0)
            retval = REDIS_ERR;

        if (retval == REDIS_OK) {
            size_t private_dirty = zmalloc_get_private_dirty();

            if (private_dirty) {
                redisLog(REDIS_NOTICE,
                    "RDB: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }

            /* If we are returning OK, at least one slave was served
             * with the RDB file as expected, so we need to send a report
             * to the parent via the pipe. The format of the message is:
             *
             * <len> <slave[0].id> <slave[0].error> ...
             *
             * len, slave IDs, and slave errors, are all uint64_t integers,
             * so basically the reply is composed of 64 bits for the len field
             * plus 2 additional 64 bit integers for each entry, for a total
             * of 'len' entries.
             *
             * The 'id' represents the slave's client ID, so that the master
             * can match the report with a specific slave, and 'error' is
             * set to 0 if the replication process terminated with a success
             * or the error code if an error occurred. */
            void *msg = zmalloc(sizeof(uint64_t)*(1+2*numfds));
            uint64_t *len = msg;
            uint64_t *ids = len+1;
            int j, msglen;

            *len = numfds;
            for (j = 0; j < numfds; j++) {
                *ids++ = clientids[j];
                *ids++ = slave_sockets.io.fdset.state[j];
            }

            /* Write the message to the parent. If we have no good slaves or
             * we are unable to transfer the message to the parent, we exit
             * with an error so that the parent will abort the replication
             * process with all the childre that were waiting. */
            msglen = sizeof(uint64_t)*(1+2*numfds);
            if (*len == 0 ||
                write(server.rdb_pipe_write_result_to_parent,msg,msglen)//通知父进程
                != msglen)
            {
                retval = REDIS_ERR;
            }
            zfree(msg);
        }
        zfree(clientids);
        exitFromChild((retval == REDIS_OK) ? 0 : 1);
    } else {
        /* Parent */
        server.stat_fork_time = ustime()-start;
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
        if (childpid == -1) {
            redisLog(REDIS_WARNING,"Can't save in background: fork: %s",
                strerror(errno));

            /* Undo the state change. The caller will perform cleanup on
             * all the slaves in BGSAVE_START state, but an early call to
             * replicationSetupSlaveForFullResync() turned it into BGSAVE_END */
            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                redisClient *slave = ln->value;
                int j;

                for (j = 0; j < numfds; j++) {
                    if (slave->id == clientids[j]) {
                        slave->replstate = REDIS_REPL_WAIT_BGSAVE_START;
                        break;
                    }
                }
            }
            close(pipefds[0]);
            close(pipefds[1]);
        } else {
            redisLog(REDIS_NOTICE,"Background RDB transfer started by pid %d",
                childpid);
            server.rdb_save_time_start = time(NULL);
            server.rdb_child_pid = childpid;
            server.rdb_child_type = REDIS_RDB_CHILD_TYPE_SOCKET;
            updateDictResizePolicy();
        }
        zfree(clientids);
        zfree(fds);
        return (childpid == -1) ? REDIS_ERR : REDIS_OK;
    }
    return REDIS_OK; /* Unreached. */
}

void saveCommand(redisClient *c) {//
    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");
        return;
    }
    if (rdbSave(server.rdb_filename) == REDIS_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}

void bgsaveCommand(redisClient *c) {//备份
    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");
    } else if (server.aof_child_pid != -1) {
        addReplyError(c,"Can't BGSAVE while AOF log rewriting is in progress");
    } else if (rdbSaveBackground(server.rdb_filename) == REDIS_OK) {
        addReplyStatus(c,"Background saving started");
    } else {
        addReply(c,shared.err);
    }
}
