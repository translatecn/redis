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

#include "over-server.h"
#include <math.h> /* isnan(), isinf() */

int getGenericCommand(client *c);

static int checkStringLength(client *c, long long size) {
    if (!mustObeyClient(c) && size > server.proto_max_bulk_len) {
        addReplyError(c, "string exceeds maximum allowed size (proto-max-bulk-len)");
        return C_ERR;
    }
    return C_OK;
}

/* setGenericCommand()函数实现了不同的SET操作
*选项和变体.调用这个函数是为了实现以下命令:SET, SETEX, PSETEX, SETNX, GETSET.
＊
* 'flags'改变命令的行为(NX, XX或GET,见下文).
'expire'表示用户通过一个Redis对象的形式设置的过期时间.它根据指定的“单位”进行解释.

* 'ok_reply'和'abort_reply'是该函数在执行操作或者不是因为NX或XX标志时将回复给客户端的内容.
＊
*如果ok_reply是NULL“+OK”被使用.
*如果abort_reply为NULL,则使用“$-1”.*/

#define OBJ_NO_FLAGS 0
#define OBJ_SET_NX (1 << 0)  // 设置 如果key不存在
#define OBJ_SET_XX (1 << 1)  // 设置 如果key存在
#define OBJ_EX (1 << 2)      // 如果给出了以秒为单位的时间,则设置
#define OBJ_PX (1 << 3)      // 设置时间单位为ms
#define OBJ_KEEPTTL (1 << 4) // 设置,并保存ttl
#define OBJ_SET_GET (1 << 5) // 设置, 并返回之前的值
#define OBJ_EXAT (1 << 6)    // 设置在某一时刻过期   秒
#define OBJ_PXAT (1 << 7)    // 设置在某一时刻过期   毫秒
#define OBJ_PERSIST (1 << 8) // 设置,如果我们需要删除ttl

static int getExpireMillisecondsOrReply(client *c, robj *expire, int flags, int unit, long long *milliseconds);

void setGenericCommand(client *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    long long milliseconds = 0; // 过期时间、   可以是间隔 、过期点
    int found = 0;
    int setkey_flags = 0;
    // unit  毫秒、秒      expire 时间长度字符串
    if (expire && getExpireMillisecondsOrReply(c, expire, flags, unit, &milliseconds) != C_OK) {
        return;
    }

    if (flags & OBJ_SET_GET) {               // set 命令中有get
        if (getGenericCommand(c) == C_ERR) { // setGenericCommand  , 直接给client写了数据
            return;
        }
    }

    found = (lookupKeyWrite(c->db, key) != NULL);

    if ((flags & OBJ_SET_NX && found) || (flags & OBJ_SET_XX && !found)) {
        if (!(flags & OBJ_SET_GET)) {
            addReply(c, abort_reply ? abort_reply : shared.null[c->resp]);
        }
        return;
    }

    setkey_flags |= (flags & OBJ_KEEPTTL) ? SETKEY_KEEPTTL : 0;
    setkey_flags |= found ? SETKEY_ALREADY_EXIST : SETKEY_DOESNT_EXIST;

    setKey(c, c->db, key, val, setkey_flags);
    server.dirty++;
    notifyKeyspaceEvent(NOTIFY_STRING, "set", key, c->db->id);

    if (expire) {
        setExpire(c, c->db, key, milliseconds);
        /* Propagate as SET Key Value PXAT millisecond-timestamp if there is
         * EX/PX/EXAT/PXAT flag. */
        robj *milliseconds_obj = createStringObjectFromLongLong(milliseconds);
        rewriteClientCommandVector(c, 5, shared.set, key, val, shared.pxat, milliseconds_obj);
        decrRefCount(milliseconds_obj);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "expire", key, c->db->id);
    }

    if (!(flags & OBJ_SET_GET)) {
        addReply(c, ok_reply ? ok_reply : shared.ok);
    }

    /* Propagate without the GET argument (Isn't needed if we had expire since in that case we completely re-written the command argv) */
    if ((flags & OBJ_SET_GET) && !expire) {
        int argc = 0;
        int j;
        robj **argv = zmalloc((c->argc - 1) * sizeof(robj *));
        for (j = 0; j < c->argc; j++) {
            char *a = c->argv[j]->ptr;
            /* Skip GET which may be repeated multiple times. */
            if (j >= 3 && (a[0] == 'g' || a[0] == 'G') && (a[1] == 'e' || a[1] == 'E') && (a[2] == 't' || a[2] == 'T') && a[3] == '\0')
                continue;
            argv[argc++] = c->argv[j];
            incrRefCount(c->argv[j]);
        }
        replaceClientCommandVector(c, argc, argv);
    }
}
// 返回过期点 的毫秒数,  如果出错,直接返回错误
static int getExpireMillisecondsOrReply(client *c, robj *expire, int flags, int unit, long long *milliseconds) {
    int ret = getLongLongFromObjectOrReply(c, expire, milliseconds, NULL);
    if (ret != C_OK) {
        return ret;
    }

    if (*milliseconds <= 0 || (unit == UNIT_SECONDS && *milliseconds > LLONG_MAX / 1000)) {
        // 提供负数,否则乘法会溢出.
        addReplyErrorExpireTime(c);
        return C_ERR;
    }

    if (unit == UNIT_SECONDS) {
        *milliseconds *= 1000;
    }

    if ((flags & OBJ_PX) || (flags & OBJ_EX)) {
        *milliseconds += mstime(); // 过期的绝对时间
    }

    if (*milliseconds <= 0) {
        addReplyErrorExpireTime(c);
        return C_ERR;
    }

    return C_OK;
}

#define COMMAND_GET 0
#define COMMAND_SET 1

// Get 特定的命令 - PERSIST/DEL
// Set 特定的命令 - XX/NX/GET
//   一般的命令 - EX/EXAT/PX/PXAT/KEEPTTL
// 对SET和GET命令中使用的扩展字符串参数执行通用验证.
int parseExtendedStringArgumentsOrReply(client *c, int *flags, int *unit, robj **expire, int command_type) {
    int j = command_type == COMMAND_GET ? 2 : 3; // get是俩参数、set是三参数
    for (; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        robj *next = (j == c->argc - 1) ? NULL : c->argv[j + 1]; // 有没有next对象

        // 设置了NX, 且没有XX
        if ((opt[0] == 'n' || opt[0] == 'N') && (opt[1] == 'x' || opt[1] == 'X') && opt[2] == '\0' && !(*flags & OBJ_SET_XX) && (command_type == COMMAND_SET)) {
            *flags |= OBJ_SET_NX;
        }
        // 设置了XX, 且没有NX
        else if ((opt[0] == 'x' || opt[0] == 'X') && (opt[1] == 'x' || opt[1] == 'X') && opt[2] == '\0' && !(*flags & OBJ_SET_NX) && (command_type == COMMAND_SET)) {
            *flags |= OBJ_SET_XX;
        }
        // 设置了GET
        else if ((opt[0] == 'g' || opt[0] == 'G') && (opt[1] == 'e' || opt[1] == 'E') && (opt[2] == 't' || opt[2] == 'T') && opt[3] == '\0' && (command_type == COMMAND_SET)) {
            *flags |= OBJ_SET_GET;
        }
        // 设置了 KEEPTTL
        else if (!strcasecmp(opt, "KEEPTTL") && !(*flags & OBJ_PERSIST) && !(*flags & OBJ_EX) && !(*flags & OBJ_EXAT) && !(*flags & OBJ_PX) && !(*flags & OBJ_PXAT) && (command_type == COMMAND_SET)) {
            *flags |= OBJ_KEEPTTL;
        }
        // 设置了 PERSIST
        else if (!strcasecmp(opt, "PERSIST") && (command_type == COMMAND_GET) && !(*flags & OBJ_EX) && !(*flags & OBJ_EXAT) && !(*flags & OBJ_PX) && !(*flags & OBJ_PXAT) && !(*flags & OBJ_KEEPTTL)) {
            *flags |= OBJ_PERSIST;
        }
        // 设置了EX, 时间单位为秒
        else if ((opt[0] == 'e' || opt[0] == 'E') && (opt[1] == 'x' || opt[1] == 'X') && opt[2] == '\0' && !(*flags & OBJ_KEEPTTL) && !(*flags & OBJ_PERSIST) && !(*flags & OBJ_EXAT) && !(*flags & OBJ_PX) && !(*flags & OBJ_PXAT) && next) {
            *flags |= OBJ_EX;
            *expire = next;
            j++;
        }
        // 设置了 PX, 时间单位为 毫秒
        else if ((opt[0] == 'p' || opt[0] == 'P') && (opt[1] == 'x' || opt[1] == 'X') && opt[2] == '\0' && !(*flags & OBJ_KEEPTTL) && !(*flags & OBJ_PERSIST) && !(*flags & OBJ_EX) && !(*flags & OBJ_EXAT) && !(*flags & OBJ_PXAT) && next) {
            *flags |= OBJ_PX;
            *unit = UNIT_MILLISECONDS;
            *expire = next;
            j++;
        }
        // 设置了 EXAT, 时间单位为 秒
        else if ((opt[0] == 'e' || opt[0] == 'E') && (opt[1] == 'x' || opt[1] == 'X') && (opt[2] == 'a' || opt[2] == 'A') && (opt[3] == 't' || opt[3] == 'T') && opt[4] == '\0' && !(*flags & OBJ_KEEPTTL) && !(*flags & OBJ_PERSIST) && !(*flags & OBJ_EX) && !(*flags & OBJ_PX) && !(*flags & OBJ_PXAT) && next) {
            *flags |= OBJ_EXAT;
            *expire = next;
            j++;
        }
        // 设置了 PXAT, 时间单位为 毫秒
        else if ((opt[0] == 'p' || opt[0] == 'P') && (opt[1] == 'x' || opt[1] == 'X') && (opt[2] == 'a' || opt[2] == 'A') && (opt[3] == 't' || opt[3] == 'T') && opt[4] == '\0' && !(*flags & OBJ_KEEPTTL) && !(*flags & OBJ_PERSIST) && !(*flags & OBJ_EX) && !(*flags & OBJ_EXAT) && !(*flags & OBJ_PX) && next) {
            *flags |= OBJ_PXAT;
            *unit = UNIT_MILLISECONDS;
            *expire = next;
            j++;
        }
        else {
            addReplyErrorObject(c, shared.syntaxerr); // -ERR 语法错误\r\n
            return C_ERR;
        }
    }
    return C_OK;
}

// SET key value [NX] [XX] [KEEPTTL] [GET] [EX <seconds>] [PX <milliseconds>] [EXAT <seconds-timestamp>][PXAT <milliseconds-timestamp>]
void setCommand(client *c) {
    // "*4\r\n$3\r\nset\r\n$1\r\na\r\n$1\r\nb\r\n$3\r\nget\r\n"
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = OBJ_NO_FLAGS; // 设置该命令 都有哪些参数

    if (parseExtendedStringArgumentsOrReply(c, &flags, &unit, &expire, COMMAND_SET) != C_OK) {
        return;
    }
    // 尝试对值对象进行编码
    c->argv[2] = tryObjectEncoding(c->argv[2]);                                    // set key value
    setGenericCommand(c, flags, c->argv[1], c->argv[2], expire, unit, NULL, NULL); // set key value
}

void setnxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c, OBJ_SET_NX, c->argv[1], c->argv[2], NULL, 0, shared.cone, shared.czero);
}

void setexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c, OBJ_EX, c->argv[1], c->argv[3], c->argv[2], UNIT_SECONDS, NULL, NULL);
}

void psetexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c, OBJ_PX, c->argv[1], c->argv[3], c->argv[2], UNIT_MILLISECONDS, NULL, NULL);
}

// 获取key的值
int getGenericCommand(client *c) {
    robj *o;
    // 尝试从数据库中取出键 c->argv[1] 对应的值对象
    // 如果键不存在时,向客户端发送回复信息,并返回 NULL
    o = lookupKeyReadOrReply(c, c->argv[1], shared.null[c->resp]);
    if (o == NULL) {
        return C_OK;
    }
    // 值对象存在,检查它的类型
    if (checkType(c, o, OBJ_STRING)) {
        return C_ERR; // 类型错误
    }
    // 类型正确,向客户端返回对象的值
    addReplyBulk(c, o);
    return C_OK;
}

void getCommand(client *c) {
    getGenericCommand(c);
}

/*
 * GETEX <key> [PERSIST][EX seconds][PX milliseconds][EXAT seconds-timestamp][PXAT milliseconds-timestamp]
 *
 * The getexCommand() function implements extended options and variants of the GET command. Unlike GET
 * command this command is not read-only.
 *
 * The default behavior when no options are specified is same as GET and does not alter any TTL.
 *
 * Only one of the below options can be used at a given time.
 *
 * 1. PERSIST removes any TTL associated with the key.
 * 2. EX Set expiry TTL in seconds.
 * 3. PX Set expiry TTL in milliseconds.
 * 4. EXAT Same like EX instead of specifying the number of seconds representing the TTL
 *      (time to live), it takes an absolute Unix timestamp
 * 5. PXAT Same like PX instead of specifying the number of milliseconds representing the TTL
 *      (time to live), it takes an absolute Unix timestamp
 *
 * Command would either return the bulk string, error or nil.
 */
void getexCommand(client *c) {
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = OBJ_NO_FLAGS;

    if (parseExtendedStringArgumentsOrReply(c, &flags, &unit, &expire, COMMAND_GET) != C_OK) {
        return;
    }

    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.null[c->resp])) == NULL)
        return;

    if (checkType(c, o, OBJ_STRING)) {
        return;
    }

    /* Validate the expiration time value first */
    long long milliseconds = 0;
    if (expire && getExpireMillisecondsOrReply(c, expire, flags, unit, &milliseconds) != C_OK) {
        return;
    }

    /* We need to do this before we expire the key or delete it */
    addReplyBulk(c, o);

    /* This command is never propagated as is. It is either propagated as PEXPIRE[AT],DEL,UNLINK or PERSIST.
     * This why it doesn't need special handling in feedAppendOnlyFile to convert relative expire time to absolute one. */
    if (((flags & OBJ_PXAT) || (flags & OBJ_EXAT)) && checkAlreadyExpired(milliseconds)) {
        /* When PXAT/EXAT absolute timestamp is specified, there can be a chance that timestamp
         * has already elapsed so delete the key in that case. */
        int deleted = server.lazyfree_lazy_expire ? dbAsyncDelete(c->db, c->argv[1]) : dbSyncDelete(c->db, c->argv[1]);
        serverAssert(deleted);
        robj *aux = server.lazyfree_lazy_expire ? shared.unlink : shared.del;
        rewriteClientCommandVector(c, 2, aux, c->argv[1]);
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty++;
    }
    else if (expire) {
        setExpire(c, c->db, c->argv[1], milliseconds);
        /* Propagate as PXEXPIREAT millisecond-timestamp if there is
         * EX/PX/EXAT/PXAT flag and the key has not expired. */
        robj *milliseconds_obj = createStringObjectFromLongLong(milliseconds);
        rewriteClientCommandVector(c, 3, shared.pexpireat, c->argv[1], milliseconds_obj);
        decrRefCount(milliseconds_obj);
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "expire", c->argv[1], c->db->id);
        server.dirty++;
    }
    else if (flags & OBJ_PERSIST) {
        if (removeExpire(c->db, c->argv[1])) {
            signalModifiedKey(c, c->db, c->argv[1]);
            rewriteClientCommandVector(c, 2, shared.persist, c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_GENERIC, "persist", c->argv[1], c->db->id);
            server.dirty++;
        }
    }
}

void getdelCommand(client *c) {
    if (getGenericCommand(c) == C_ERR)
        return;
    int deleted = server.lazyfree_lazy_user_del ? dbAsyncDelete(c->db, c->argv[1]) : dbSyncDelete(c->db, c->argv[1]);
    if (deleted) {
        /* Propagate as DEL/UNLINK command */
        robj *aux = server.lazyfree_lazy_user_del ? shared.unlink : shared.del;
        rewriteClientCommandVector(c, 2, aux, c->argv[1]);
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty++;
    }
}

void getsetCommand(client *c) {
    if (getGenericCommand(c) == C_ERR)
        return;
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setKey(c, c->db, c->argv[1], c->argv[2], 0);
    notifyKeyspaceEvent(NOTIFY_STRING, "set", c->argv[1], c->db->id);
    server.dirty++;

    /* Propagate as SET command */
    rewriteClientCommandArgument(c, 0, shared.set);
}

void setrangeCommand(client *c) {
    robj *o;
    long offset;
    sds value = c->argv[3]->ptr;

    if (getLongFromObjectOrReply(c, c->argv[2], &offset, NULL) != C_OK)
        return;

    if (offset < 0) {
        addReplyError(c, "offset is out of range");
        return;
    }

    o = lookupKeyWrite(c->db, c->argv[1]);
    if (o == NULL) {
        /* Return 0 when setting nothing on a non-existing string */
        if (sdslen(value) == 0) {
            addReply(c, shared.czero);
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(c, offset + sdslen(value)) != C_OK)
            return;

        o = createObject(OBJ_STRING, sdsnewlen(NULL, offset + sdslen(value)));
        dbAdd(c->db, c->argv[1], o);
    }
    else {
        size_t olen;

        /* Key exists, check type */
        if (checkType(c, o, OBJ_STRING))
            return;

        /* Return existing string length when setting nothing */
        olen = stringObjectLen(o);
        if (sdslen(value) == 0) {
            addReplyLongLong(c, olen);
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(c, offset + sdslen(value)) != C_OK)
            return;

        /* Create a copy when the object is shared or encoded. */
        o = dbUnshareStringValue(c->db, c->argv[1], o);
    }

    if (sdslen(value) > 0) {
        o->ptr = sdsgrowzero(o->ptr, offset + sdslen(value));
        memcpy((char *)o->ptr + offset, value, sdslen(value));
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STRING, "setrange", c->argv[1], c->db->id);
        server.dirty++;
    }
    addReplyLongLong(c, sdslen(o->ptr));
}

void getrangeCommand(client *c) {
    robj *o;
    long long start, end;
    char *str, llbuf[32];
    size_t strlen;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK)
        return;
    if (getLongLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)
        return;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.emptybulk)) == NULL || checkType(c, o, OBJ_STRING))
        return;

    if (o->encoding == OBJ_ENCODING_INT) {
        str = llbuf;
        strlen = ll2string(llbuf, sizeof(llbuf), (long)o->ptr);
    }
    else {
        str = o->ptr;
        strlen = sdslen(str);
    }

    /* Convert negative indexes */
    if (start < 0 && end < 0 && start > end) {
        addReply(c, shared.emptybulk);
        return;
    }
    if (start < 0)
        start = strlen + start;
    if (end < 0)
        end = strlen + end;
    if (start < 0)
        start = 0;
    if (end < 0)
        end = 0;
    if ((unsigned long long)end >= strlen)
        end = strlen - 1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) {
        addReply(c, shared.emptybulk);
    }
    else {
        addReplyBulkCBuffer(c, (char *)str + start, end - start + 1);
    }
}

void mgetCommand(client *c) {
    int j;

    addReplyArrayLen(c, c->argc - 1);
    for (j = 1; j < c->argc; j++) {
        robj *o = lookupKeyRead(c->db, c->argv[j]);
        if (o == NULL) {
            addReplyNull(c);
        }
        else {
            if (o->type != OBJ_STRING) {
                addReplyNull(c);
            }
            else {
                addReplyBulk(c, o);
            }
        }
    }
}

void msetGenericCommand(client *c, int nx) {
    int j;

    if ((c->argc % 2) == 0) {
        addReplyErrorArity(c);
        return;
    }

    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set anything if at least one key already exists. */
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {
            if (lookupKeyWrite(c->db, c->argv[j]) != NULL) {
                addReply(c, shared.czero);
                return;
            }
        }
    }

    for (j = 1; j < c->argc; j += 2) {
        c->argv[j + 1] = tryObjectEncoding(c->argv[j + 1]);
        setKey(c, c->db, c->argv[j], c->argv[j + 1], 0);
        notifyKeyspaceEvent(NOTIFY_STRING, "set", c->argv[j], c->db->id);
    }
    server.dirty += (c->argc - 1) / 2;
    addReply(c, nx ? shared.cone : shared.ok);
}

void msetCommand(client *c) {
    msetGenericCommand(c, 0);
}

void msetnxCommand(client *c) {
    msetGenericCommand(c, 1);
}

void incrDecrCommand(client *c, long long incr) {
    long long value, oldvalue;
    robj *o, *new;

    o = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_STRING))
        return;
    if (getLongLongFromObjectOrReply(c, o, &value, NULL) != C_OK)
        return;

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN - oldvalue)) || (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX - oldvalue))) {
        addReplyError(c, "increment or decrement would overflow");
        return;
    }
    value += incr;

    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT && (value < 0 || value >= OBJ_SHARED_INTEGERS) && value >= LONG_MIN && value <= LONG_MAX) {
        new = o;
        o->ptr = (void *)((long)value);
    }
    else {
        new = createStringObjectFromLongLongForValue(value);
        if (o) {
            dbOverwrite(c->db, c->argv[1], new);
        }
        else {
            dbAdd(c->db, c->argv[1], new);
        }
    }
    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING, "incrby", c->argv[1], c->db->id);
    server.dirty++;
    addReplyLongLong(c, value);
}

void incrCommand(client *c) {
    incrDecrCommand(c, 1);
}

void decrCommand(client *c) {
    incrDecrCommand(c, -1);
}

void incrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK)
        return;
    incrDecrCommand(c, incr);
}

void decrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK)
        return;
    /* Overflow check: negating LLONG_MIN will cause an overflow */
    if (incr == LLONG_MIN) {
        addReplyError(c, "decrement would overflow");
        return;
    }
    incrDecrCommand(c, -incr);
}

void incrbyfloatCommand(client *c) {
    long double incr, value;
    robj *o, *new;

    o = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_STRING))
        return;
    if (getLongDoubleFromObjectOrReply(c, o, &value, NULL) != C_OK || getLongDoubleFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK)
        return;

    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c, "increment would produce NaN or Infinity");
        return;
    }
    new = createStringObjectFromLongDouble(value, 1);
    if (o)
        dbOverwrite(c->db, c->argv[1], new);
    else
        dbAdd(c->db, c->argv[1], new);
    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING, "incrbyfloat", c->argv[1], c->db->id);
    server.dirty++;
    addReplyBulk(c, new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    rewriteClientCommandArgument(c, 0, shared.set);
    rewriteClientCommandArgument(c, 2, new);
    rewriteClientCommandArgument(c, 3, shared.keepttl);
}

void appendCommand(client *c) {
    size_t totlen;
    robj *o, *append;

    o = lookupKeyWrite(c->db, c->argv[1]);
    if (o == NULL) {
        /* Create the key */
        c->argv[2] = tryObjectEncoding(c->argv[2]);
        dbAdd(c->db, c->argv[1], c->argv[2]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    }
    else {
        /* Key exists, check type */
        if (checkType(c, o, OBJ_STRING))
            return;

        /* "append" is an argument, so always an sds */
        append = c->argv[2];
        totlen = stringObjectLen(o) + sdslen(append->ptr);
        if (checkStringLength(c, totlen) != C_OK)
            return;

        /* Append the value */
        o = dbUnshareStringValue(c->db, c->argv[1], o);
        o->ptr = sdscatlen(o->ptr, append->ptr, sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }
    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING, "append", c->argv[1], c->db->id);
    server.dirty++;
    addReplyLongLong(c, totlen);
}

void strlenCommand(client *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_STRING))
        return;
    addReplyLongLong(c, stringObjectLen(o));
}

/* LCS key1 key2 [LEN] [IDX] [MINMATCHLEN <len>] [WITHMATCHLEN] */
void lcsCommand(client *c) {
    uint32_t i, j;
    long long minmatchlen = 0;
    sds a = NULL, b = NULL;
    int getlen = 0, getidx = 0, withmatchlen = 0;
    robj *obja = NULL, *objb = NULL;

    obja = lookupKeyRead(c->db, c->argv[1]);
    objb = lookupKeyRead(c->db, c->argv[2]);
    if ((obja && obja->type != OBJ_STRING) || (objb && objb->type != OBJ_STRING)) {
        addReplyError(c, "The specified keys must contain string values");
        /* Don't cleanup the objects, we need to do that
         * only after calling getDecodedObject(). */
        obja = NULL;
        objb = NULL;
        goto cleanup;
    }
    obja = obja ? getDecodedObject(obja) : createStringObject("", 0);
    objb = objb ? getDecodedObject(objb) : createStringObject("", 0);
    a = obja->ptr;
    b = objb->ptr;

    for (j = 3; j < (uint32_t)c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc - 1) - j;

        if (!strcasecmp(opt, "IDX")) {
            getidx = 1;
        }
        else if (!strcasecmp(opt, "LEN")) {
            getlen = 1;
        }
        else if (!strcasecmp(opt, "WITHMATCHLEN")) {
            withmatchlen = 1;
        }
        else if (!strcasecmp(opt, "MINMATCHLEN") && moreargs) {
            if (getLongLongFromObjectOrReply(c, c->argv[j + 1], &minmatchlen, NULL) != C_OK)
                goto cleanup;
            if (minmatchlen < 0)
                minmatchlen = 0;
            j++;
        }
        else {
            addReplyErrorObject(c, shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Complain if the user passed ambiguous parameters. */
    if (getlen && getidx) {
        addReplyError(c, "If you want both the length and indexes, please just use IDX.");
        goto cleanup;
    }

    /* Detect string truncation or later overflows. */
    if (sdslen(a) >= UINT32_MAX - 1 || sdslen(b) >= UINT32_MAX - 1) {
        addReplyError(c, "String too long for LCS");
        goto cleanup;
    }

    /* Compute the LCS using the vanilla dynamic programming technique of
     * building a table of LCS(x,y) substrings. */
    uint32_t alen = sdslen(a);
    uint32_t blen = sdslen(b);

    /* Setup an uint32_t array to store at LCS[i,j] the length of the
     * LCS A0..i-1, B0..j-1. Note that we have a linear array here, so
     * we index it as LCS[j+(blen+1)*i] */
#define LCS(A, B) lcs[(B) + ((A) * (blen + 1))]

    /* Try to allocate the LCS table, and abort on overflow or insufficient memory. */
    unsigned long long lcssize = (unsigned long long)(alen + 1) * (blen + 1); /* Can't overflow due to the size limits above. */
    unsigned long long lcsalloc = lcssize * sizeof(uint32_t);
    uint32_t *lcs = NULL;
    if (lcsalloc < SIZE_MAX && lcsalloc / lcssize == sizeof(uint32_t)) {
        if (lcsalloc > (size_t)server.proto_max_bulk_len) {
            addReplyError(c, "Insufficient memory, transient memory for LCS exceeds proto-max-bulk-len");
            goto cleanup;
        }
        lcs = ztrymalloc(lcsalloc);
    }
    if (!lcs) {
        addReplyError(c, "Insufficient memory, failed allocating transient memory for LCS");
        goto cleanup;
    }

    /* Start building the LCS table. */
    for (uint32_t i = 0; i <= alen; i++) {
        for (uint32_t j = 0; j <= blen; j++) {
            if (i == 0 || j == 0) {
                /* If one substring has length of zero, the
                 * LCS length is zero. */
                LCS(i, j) = 0;
            }
            else if (a[i - 1] == b[j - 1]) {
                /* The len LCS (and the LCS itself) of two
                 * sequences with the same final character, is the
                 * LCS of the two sequences without the last char
                 * plus that last char. */
                LCS(i, j) = LCS(i - 1, j - 1) + 1;
            }
            else {
                /* If the last character is different, take the longest
                 * between the LCS of the first string and the second
                 * minus the last char, and the reverse. */
                uint32_t lcs1 = LCS(i - 1, j);
                uint32_t lcs2 = LCS(i, j - 1);
                LCS(i, j) = lcs1 > lcs2 ? lcs1 : lcs2;
            }
        }
    }

    /* Store the actual LCS string in "result" if needed. We create
     * it backward, but the length is already known, we store it into idx. */
    uint32_t idx = LCS(alen, blen);
    sds result = NULL;            /* Resulting LCS string. */
    void *arraylenptr = NULL;     /* Deferred length of the array for IDX. */
    uint32_t arange_start = alen, /* alen signals that values are not set. */
        arange_end = 0, brange_start = 0, brange_end = 0;

    /* Do we need to compute the actual LCS string? Allocate it in that case. */
    int computelcs = getidx || !getlen;
    if (computelcs)
        result = sdsnewlen(SDS_NOINIT, idx);

    /* Start with a deferred array if we have to emit the ranges. */
    uint32_t arraylen = 0; /* Number of ranges emitted in the array. */
    if (getidx) {
        addReplyMapLen(c, 2);
        addReplyBulkCString(c, "matches");
        arraylenptr = addReplyDeferredLen(c);
    }

    i = alen, j = blen;
    while (computelcs && i > 0 && j > 0) {
        int emit_range = 0;
        if (a[i - 1] == b[j - 1]) {
            /* If there is a match, store the character and reduce
             * the indexes to look for a new match. */
            result[idx - 1] = a[i - 1];

            /* Track the current range. */
            if (arange_start == alen) {
                arange_start = i - 1;
                arange_end = i - 1;
                brange_start = j - 1;
                brange_end = j - 1;
            }
            else {
                /* Let's see if we can extend the range backward since
                 * it is contiguous. */
                if (arange_start == i && brange_start == j) {
                    arange_start--;
                    brange_start--;
                }
                else {
                    emit_range = 1;
                }
            }
            /* Emit the range if we matched with the first byte of
             * one of the two strings. We'll exit the loop ASAP. */
            if (arange_start == 0 || brange_start == 0)
                emit_range = 1;
            idx--;
            i--;
            j--;
        }
        else {
            /* Otherwise reduce i and j depending on the largest
             * LCS between, to understand what direction we need to go. */
            uint32_t lcs1 = LCS(i - 1, j);
            uint32_t lcs2 = LCS(i, j - 1);
            if (lcs1 > lcs2)
                i--;
            else
                j--;
            if (arange_start != alen)
                emit_range = 1;
        }

        /* Emit the current range if needed. */
        uint32_t match_len = arange_end - arange_start + 1;
        if (emit_range) {
            if (minmatchlen == 0 || match_len >= minmatchlen) {
                if (arraylenptr) {
                    addReplyArrayLen(c, 2 + withmatchlen);
                    addReplyArrayLen(c, 2);
                    addReplyLongLong(c, arange_start);
                    addReplyLongLong(c, arange_end);
                    addReplyArrayLen(c, 2);
                    addReplyLongLong(c, brange_start);
                    addReplyLongLong(c, brange_end);
                    if (withmatchlen)
                        addReplyLongLong(c, match_len);
                    arraylen++;
                }
            }
            arange_start = alen; /* Restart at the next match. */
        }
    }

    /* Signal modified key, increment dirty, ... */

    /* Reply depending on the given options. */
    if (arraylenptr) {
        addReplyBulkCString(c, "len");
        addReplyLongLong(c, LCS(alen, blen));
        setDeferredArrayLen(c, arraylenptr, arraylen);
    }
    else if (getlen) {
        addReplyLongLong(c, LCS(alen, blen));
    }
    else {
        addReplyBulkSds(c, result);
        result = NULL;
    }

    /* Cleanup. */
    sdsfree(result);
    zfree(lcs);

cleanup:
    if (obja)
        decrRefCount(obja);
    if (objb)
        decrRefCount(objb);
    return;
}
