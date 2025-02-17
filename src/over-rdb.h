// https://www.5ycode.com/article/1190.html
#ifndef __RDB_H
#define __RDB_H

#include <stdio.h>
#include "rio.h"

#include "over-server.h"

#define RDB_VERSION 10 // 当前rdb格式版本

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|XXXXXX => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|XXXXXX XXXXXXXX =>  01, the len is 14 bits, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => A full 32 bit len in net byte order will follow
 * 10|000001 [64 bit integer] => A full 64 bit len in net byte order will follow
 * 11|OBKIND this means: specially encoded object will follow. The six bits
 *           number specify the kind of object that follows.
 *           See the RDB_ENC_* defines.
 *
 * Lengths up to 63 are stored using a single byte, most DB keys, and may
 * values, will fit inside. */
#define RDB_6BITLEN 0
#define RDB_14BITLEN 1
#define RDB_32BITLEN 0x80
#define RDB_64BITLEN 0x81
#define RDB_ENCVAL 3
#define RDB_LENERR UINT64_MAX

/* When a length of a string object stored on disk has the first two bits
 * set, the remaining six bits specify a special encoding for the object
 * accordingly to the following defines: */
#define RDB_ENC_INT8 0  /* 8 bit signed integer */
#define RDB_ENC_INT16 1 /* 16 bit signed integer */
#define RDB_ENC_INT32 2 /* 32 bit signed integer */
#define RDB_ENC_LZF 3   /* string compressed with FASTLZ */

/* Map object types to RDB object types. Macros starting with OBJ_ are for
 * memory storage and may change. Instead RDB types must be fixed because
 * we store them on disk. */
#define RDB_TYPE_STRING 0
#define RDB_TYPE_LIST 1
#define RDB_TYPE_SET 2
#define RDB_TYPE_ZSET 3
#define RDB_TYPE_HASH 4
#define RDB_TYPE_ZSET_2 5 /* ZSET version 2 with doubles stored in binary. */
#define RDB_TYPE_MODULE 6
#define RDB_TYPE_MODULE_2                                  \
    7 /* Module value with annotations for parsing without \
         the generating module being loaded. */
/* NOTE: WHEN ADDING NEW RDB TYPE, UPDATE rdbIsObjectType() BELOW */

/* Object types for encoded objects. */
#define RDB_TYPE_HASH_ZIPMAP 9
#define RDB_TYPE_LIST_ZIPLIST 10
#define RDB_TYPE_SET_INTSET 11
#define RDB_TYPE_ZSET_ZIPLIST 12
#define RDB_TYPE_HASH_ZIPLIST 13
#define RDB_TYPE_LIST_QUICKLIST 14
#define RDB_TYPE_STREAM_LISTPACKS 15
#define RDB_TYPE_HASH_LISTPACK 16
#define RDB_TYPE_ZSET_LISTPACK 17
#define RDB_TYPE_LIST_QUICKLIST_2 18
#define RDB_TYPE_STREAM_LISTPACKS_2 19
/* NOTE: WHEN ADDING NEW RDB TYPE, UPDATE rdbIsObjectType() BELOW */

/* Test if a type is an object type. */
#define rdbIsObjectType(t) ((t >= 0 && t <= 7) || (t >= 9 && t <= 19))

// 特殊的RDB操作码
#define RDB_OPCODE_FUNCTION2 245     /* function library data */
#define RDB_OPCODE_FUNCTION 246      /* old function library data for 7.0 rc1 and rc2 */
#define RDB_OPCODE_MODULE_AUX 247    /* Module auxiliary data. */
#define RDB_OPCODE_IDLE 248          // 标识LRU空闲时间
#define RDB_OPCODE_FREQ 249          // 标识LFU访问频率信息
#define RDB_OPCODE_AUX 250           // 标识RDB文件头的属性信息
#define RDB_OPCODE_RESIZEDB 251      /* Hash table resize hint. */
#define RDB_OPCODE_EXPIRETIME_MS 252 // 标识以毫秒记录的过期时间
#define RDB_OPCODE_EXPIRETIME 253    /* Old expire time in seconds. */
#define RDB_OPCODE_SELECTDB 254      // 标识文件中后续键值对所属的数据库编号  // DB切换
#define RDB_OPCODE_EOF 255           // 标识RDB文件结束,用在文件尾

/* Module serialized values sub opcodes */
#define RDB_MODULE_OPCODE_EOF 0    /* End of module value. */
#define RDB_MODULE_OPCODE_SINT 1   /* Signed integer. */
#define RDB_MODULE_OPCODE_UINT 2   /* Unsigned integer. */
#define RDB_MODULE_OPCODE_FLOAT 3  /* Float. */
#define RDB_MODULE_OPCODE_DOUBLE 4 /* Double. */
#define RDB_MODULE_OPCODE_STRING 5 /* String. */

/* rdbLoad...() functions flags. */
#define RDB_LOAD_NONE 0
#define RDB_LOAD_ENC (1 << 0)
#define RDB_LOAD_PLAIN (1 << 1)
#define RDB_LOAD_SDS (1 << 2)

/* flags on the purpose of rdb save or load */
#define RDBFLAGS_NONE 0                /* No special RDB loading. */
#define RDBFLAGS_AOF_PREAMBLE (1 << 0) // 加载/保存RDB作为AOF的序号
#define RDBFLAGS_REPLICATION (1 << 1)  /* Load/save for SYNC. */
#define RDBFLAGS_ALLOW_DUP (1 << 2)    /* Allow duplicated keys when loading.*/
#define RDBFLAGS_FEED_REPL (1 << 3)    /* Feed replication stream when loading.*/

/* When rdbLoadObject() returns NULL, the err flag is
 * set to hold the type of error that occurred */
#define RDB_LOAD_ERR_EMPTY_KEY 1 /* Error of empty key */
#define RDB_LOAD_ERR_OTHER 2     /* Any other errors */

int rdbSaveType(rio *rdb, unsigned char type);

int rdbLoadType(rio *rdb);

time_t rdbLoadTime(rio *rdb);

int rdbSaveLen(rio *rdb, uint64_t len);

int rdbSaveMillisecondTime(rio *rdb, long long t);

long long rdbLoadMillisecondTime(rio *rdb, int rdbver);

uint64_t rdbLoadLen(rio *rdb, int *isencoded);

int rdbLoadLenByRef(rio *rdb, int *isencoded, uint64_t *lenptr);

int rdbSaveObjectType(rio *rdb, robj *o);

int rdbLoadObjectType(rio *rdb);

int rdbLoad(char *filename, rdbSaveInfo *rsi, int rdbflags);

int rdbSaveBackground(int req, char *filename, rdbSaveInfo *rsi);

int rdbSaveToSlavesSockets(int req, rdbSaveInfo *rsi);

void rdbRemoveTempFile(pid_t childpid, int from_signal);

int rdbSave(int req, char *filename, rdbSaveInfo *rsi);

ssize_t rdbSaveObject(rio *rdb, robj *o, robj *key, int dbid);

size_t rdbSavedObjectLen(robj *o, robj *key, int dbid);

robj *rdbLoadObject(int type, rio *rdb, sds key, int dbid, int *error);

void backgroundSaveDoneHandler(int exitcode, int bysignal);

int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime, int dbid);

ssize_t rdbSaveSingleModuleAux(rio *rdb, int when, moduleType *mt);

robj *rdbLoadCheckModuleValue(rio *rdb, char *modulename);

robj *rdbLoadStringObject(rio *rdb);

ssize_t rdbSaveStringObject(rio *rdb, robj *obj);

ssize_t rdbSaveRawString(rio *rdb, unsigned char *s, size_t len);

void *rdbGenericLoadStringObject(rio *rdb, int flags, size_t *lenptr);

int rdbSaveBinaryDoubleValue(rio *rdb, double val);

int rdbLoadBinaryDoubleValue(rio *rdb, double *val);

int rdbSaveBinaryFloatValue(rio *rdb, float val);

int rdbLoadBinaryFloatValue(rio *rdb, float *val);

int rdbLoadRio(rio *rdb, int rdbflags, rdbSaveInfo *rsi);

int rdbLoadRioWithLoadingCtx(rio *rdb, int rdbflags, rdbSaveInfo *rsi, rdbLoadingCtx *rdb_loading_ctx);

int rdbFunctionLoad(rio *rdb, int ver, functionsLibCtx *lib_ctx, int type, int rdbflags, sds *err);

int rdbSaveRio(int req, rio *rdb, int *error, int rdbflags, rdbSaveInfo *rsi);

ssize_t rdbSaveFunctions(rio *rdb);

rdbSaveInfo *rdbPopulateSaveInfo(rdbSaveInfo *rsi);

#endif
