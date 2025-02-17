#include "over-server.h"
#include "slowlog.h"
#include "over-latency.h"
#include "over-monotonic.h"

void serveClientBlockedOnList(client *receiver, robj *o, robj *key, robj *dstkey, redisDb *db, int wherefrom, int whereto, int *deleted);

int getListPositionFromObjectOrReply(client *c, robj *arg, int *position);

// 这个结构表示我们存储在客户端结构中的阻塞key信息.每个客户端都有一个client.bpop.keys的dict.
// 哈希表的键是指向“robj”结构的Redis键指针.值就是这个结构.
typedef struct bkinfo {
    listNode *listnode; // db->blocking_keys[key]        c->db->blocking_keys   维护了双向的对应关系
    streamID stream_id; // 如果在流中阻塞,流ID
} bkinfo;

// 对给定的客户端进行阻塞,btype 客户端类型
void blockClient(client *c, int btype) {
    // 主客户端永远不应该被阻塞,除非暂停或模块
    serverAssert(!(c->flags & CLIENT_MASTER && btype != BLOCKED_MODULE && btype != BLOCKED_POSTPONE));

    c->flags |= CLIENT_BLOCKED;
    c->btype = btype;
    server.blocked_clients++;
    server.blocked_clients_by_type[btype]++;
    addClientToTimeoutTable(c);
    if (btype == BLOCKED_POSTPONE) {
        listAddNodeTail(server.postponed_clients, c);
        c->postponed_list_node = listLast(server.postponed_clients);
        c->flags |= CLIENT_PENDING_COMMAND;
    }
}

// 在客户端完成阻塞操作后调用此函数,以便更新总命令持续时间,如果需要将命令记录到Slow日志中,如果需要则记录回复持续时间事件.
void updateStatsOnUnblock(client *c, long blocked_us, long reply_us, int had_errors) {
    const ustime_t total_cmd_duration = c->duration + blocked_us + reply_us;
    c->lastcmd->microseconds += total_cmd_duration;
    if (had_errors) {
        c->lastcmd->failed_calls++;
    }
    if (server.latency_tracking_enabled) {
        updateCommandLatencyHistogram(&(c->lastcmd->latency_histogram), total_cmd_duration * 1000);
    }
    // 如果需要,将该命令记录到Slow日志中.
    slowlogPushCurrentCommand(c, c->lastcmd, total_cmd_duration);
    // 记录回复持续时间事件.
    latencyAddSampleIfNeeded("command-unblocking", reply_us / 1000);
}

// 取消所有在 unblocked_clients 链表中的客户端的阻塞状态
void processUnblockedClients(void) {
    listNode *ln;
    client *c;

    while (listLength(server.unblocked_clients)) {
        ln = listFirst(server.unblocked_clients);
        serverAssert(ln != NULL);
        c = ln->value;
        listDelNode(server.unblocked_clients, ln);
        c->flags &= ~CLIENT_UNBLOCKED;

        // 处理输入缓冲区中的剩余数据,除非客户端再次阻塞.
        // 实际上,processInputBuffer()在继续之前检查客户端是否被阻塞,但情况可能会发生变化,并且这种方法在概念上更正确.
        if (!(c->flags & CLIENT_BLOCKED)) {
            // 如果我们有一个排队命令,现在就执行它.
            if (processPendingCommandAndInputBuffer(c) == C_ERR) {
                c = NULL;
            }
        }
        beforeNextClient(c);
    }
}

// 取消给定的客户端的阻塞状态
void unblockClient(client *c) {
    if (c->btype == BLOCKED_LIST || c->btype == BLOCKED_ZSET || c->btype == BLOCKED_STREAM) {
        unblockClientWaitingData(c);
    }
    else if (c->btype == BLOCKED_WAIT) {
        unblockClientWaitingReplicas(c);
    }
    else if (c->btype == BLOCKED_MODULE) {
        if (moduleClientIsBlockedOnKeys(c))
            unblockClientWaitingData(c);
        unblockClientFromModule(c);
    }
    else if (c->btype == BLOCKED_POSTPONE) {
        listDelNode(server.postponed_clients, c->postponed_list_node);
        c->postponed_list_node = NULL;
    }
    else if (c->btype == BLOCKED_SHUTDOWN) { /* No special cleanup. */
    }
    else {
        serverPanic("Unknown btype in unblockClient().");
    }

    /* Reset the client for a new query since, for blocking commands
     * we do not do it immediately after the command returns (when the
     * client got blocked) in order to be still able to access the argument
     * vector from module callbacks and updateStatsOnUnblock. */
    if (c->btype != BLOCKED_POSTPONE && c->btype != BLOCKED_SHUTDOWN) {
        freeClientOriginalArgv(c);
        resetClient(c);
    }

    /* Clear the flags, and put the client in the unblocked list so that
     * we'll process new commands in its query buffer ASAP. */
    server.blocked_clients--;
    server.blocked_clients_by_type[c->btype]--;
    c->flags &= ~CLIENT_BLOCKED;
    c->btype = BLOCKED_NONE;
    removeClientFromTimeoutTable(c);
    queueClientForReprocessing(c);
}

// 该函数将安排客户端在安全的时间进行重新处理.
// 当客户端由于某些原因(阻塞操作,client PAUSE或其他原因)被阻塞时,这很有用,因为它可能会以一些累积的需要尽快处理的查询缓冲区结束:
//     1. 当一个客户端被阻塞时,它的可读处理程序仍然是活动的.
//     2. 然而,在这种情况下,它只将数据放入查询缓冲区,但查询不会被解析或执行,一旦有足够的数据继续进行(因为客户端被阻塞…所以我们不能执行命令).
//     3. 当客户端被解除阻塞时,如果没有这个函数,客户端将不得不编写一些查询,以便可读处理程序最终调用processQueryBuffer*().
//     4. 使用这个函数,我们可以将客户端放在一个队列中,以便在安全的时间执行查询.
void queueClientForReprocessing(client *c) {
// 由于之前的阻塞操作,客户端可能已经进入未阻塞列表,不要多次将其添加回列表.
    if (!(c->flags & CLIENT_UNBLOCKED)) {
        c->flags |= CLIENT_UNBLOCKED;
        listAddNodeTail(server.unblocked_clients, c);
    }
}

// 等待超时,向被阻塞的客户端返回通知
void replyToBlockedClientTimedOut(client *c) {
    if (c->btype == BLOCKED_LIST || c->btype == BLOCKED_ZSET || c->btype == BLOCKED_STREAM) {
        addReplyNullArray(c);
    }
    else if (c->btype == BLOCKED_WAIT) {
        addReplyLongLong(c, replicationCountAcksByOffset(c->bpop.reploffset));
    }
    else if (c->btype == BLOCKED_MODULE) {
        moduleBlockedClientTimedOut(c);
    }
    else {
        serverPanic("Unknown btype in replyToBlockedClientTimedOut().");
    }
}

// 如果一个或多个客户端在SHUTDOWN命令上被阻塞,该函数将向它们发送错误应答并解除阻塞.
void replyToClientsBlockedOnShutdown(void) {
    if (server.blocked_clients_by_type[BLOCKED_SHUTDOWN] == 0) // shutdown次数
        return;
    listNode *ln;
    listIter li;
    listRewind(server.clients, &li);
    while ((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        if (c->flags & CLIENT_BLOCKED && c->btype == BLOCKED_SHUTDOWN) {
            addReplyError(c, "试图关闭错误.检查日志.");
            unblockClient(c);
        }
    }
}

// 断开所有阻塞的链接
void disconnectAllBlockedClients(void) {
    listNode *ln;
    listIter li;

    listRewind(server.clients, &li);
    while ((ln = listNext(&li))) {
        client *c = listNodeValue(ln);

        if (c->flags & CLIENT_BLOCKED) {
            /* POSTPONEd clients are an exception, when they'll be unblocked, the
             * command processing will start from scratch, and the command will
             * be either executed or rejected. (unlike LIST blocked clients for
             * which the command is already in progress in a way. */
            if (c->btype == BLOCKED_POSTPONE)
                continue;

            addReplyError(
                c,
                "-UNBLOCKED force unblock from blocking operation, instance state changed (master -> replica?)");
            unblockClient(c);
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
        }
    }
}
// handleclientsblockedonkey()的辅助函数.当列表键上可能阻塞了客户端,并且可能有新的数据要获取(键已经准备好了)时,将调用此函数.
void serveClientsBlockedOnListKey(robj *o, readyList *rl) {
    if (!server.blocked_clients_by_type[BLOCKED_LIST]) // 如果list阻塞类型的计数器为0 ,直接返回
        return;

    dictEntry *de = dictFind(rl->db->blocking_keys, rl->key);
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients, &li);

        while ((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_LIST)
                continue;

            int deleted = 0;
            robj *dstkey = receiver->bpop.target;
            int wherefrom = receiver->bpop.blockpos.wherefrom;
            int whereto = receiver->bpop.blockpos.whereto;

            /* Protect receiver->bpop.target, that will be
             * freed by the next unblockClient()
             * call. */
            if (dstkey)
                incrRefCount(dstkey);

            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            serveClientBlockedOnList(receiver, o, rl->key, dstkey, rl->db, wherefrom, whereto, &deleted);
            updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
            unblockClient(receiver);
            afterCommand(receiver);
            server.current_client = old_client;

            if (dstkey)
                decrRefCount(dstkey);

            /* The list is empty and has been deleted. */
            if (deleted)
                break;
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * when there may be clients blocked on a sorted set key, and there may be new
 * data to fetch (the key is ready). */
void serveClientsBlockedOnSortedSetKey(robj *o, readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_ZSET,
     * we can skip this loop. */
    if (!server.blocked_clients_by_type[BLOCKED_ZSET])
        return;

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    dictEntry *de = dictFind(rl->db->blocking_keys, rl->key);
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients, &li);

        while ((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_ZSET)
                continue;

            int deleted = 0;
            long llen = zsetLength(o);
            long count = receiver->bpop.count;
            int where = receiver->bpop.blockpos.wherefrom;
            int use_nested_array = (receiver->lastcmd && receiver->lastcmd->proc == bzmpopCommand) ? 1 : 0;
            int reply_nil_when_empty = use_nested_array;

            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            genericZpopCommand(receiver, &rl->key, 1, where, 1, count, use_nested_array, reply_nil_when_empty, &deleted);

            /* Replicate the command. */
            int argc = 2;
            robj *argv[3];
            argv[0] = where == ZSET_MIN ? shared.zpopmin : shared.zpopmax;
            argv[1] = rl->key;
            incrRefCount(rl->key);
            if (count != -1) {
                /* Replicate it as command with COUNT. */
                robj *count_obj = createStringObjectFromLongLong((count > llen) ? llen : count);
                argv[2] = count_obj;
                argc++;
            }
            alsoPropagate(receiver->db->id, argv, argc, PROPAGATE_AOF | PROPAGATE_REPL);
            decrRefCount(argv[1]);
            if (count != -1)
                decrRefCount(argv[2]);

            updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
            unblockClient(receiver);
            afterCommand(receiver);
            server.current_client = old_client;

            /* The zset is empty and has been deleted. */
            if (deleted)
                break;
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * when there may be clients blocked on a stream key, and there may be new
 * data to fetch (the key is ready). */
void serveClientsBlockedOnStreamKey(robj *o, readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_STREAM,
     * we can skip this loop. */
    if (!server.blocked_clients_by_type[BLOCKED_STREAM])
        return;

    dictEntry *de = dictFind(rl->db->blocking_keys, rl->key);
    stream *s = o->ptr;

    /* We need to provide the new data arrived on the stream
     * to all the clients that are waiting for an offset smaller
     * than the current top item. */
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients, &li);

        while ((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_STREAM)
                continue;
            bkinfo *bki = dictFetchValue(receiver->bpop.keys, rl->key);
            streamID *gt = &bki->stream_id;

            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);

            /* If we blocked in the context of a consumer
             * group, we need to resolve the group and update the
             * last ID the client is blocked for: this is needed
             * because serving other clients in the same consumer
             * group will alter the "last ID" of the consumer
             * group, and clients blocked in a consumer group are
             * always blocked for the ">" ID: we need to deliver
             * only new messages and avoid unblocking the client
             * otherwise. */
            streamCG *group = NULL;
            if (receiver->bpop.xread_group) {
                group = streamLookupCG(s, receiver->bpop.xread_group->ptr);
                /* If the group was not found, send an error
                 * to the consumer. */
                if (!group) {
                    addReplyError(
                        receiver,
                        "-NOGROUP the consumer group this client "
                        "was blocked on no longer exists");
                    goto unblock_receiver;
                }
                else {
                    *gt = group->last_id;
                }
            }

            if (streamCompareID(&s->last_id, gt) > 0) {
                streamID start = *gt;
                streamIncrID(&start);

                /* Lookup the consumer for the group, if any. */
                streamConsumer *consumer = NULL;
                int noack = 0;

                if (group) {
                    noack = receiver->bpop.xread_group_noack;
                    sds name = receiver->bpop.xread_consumer->ptr;
                    consumer = streamLookupConsumer(group, name, SLC_DEFAULT);
                    if (consumer == NULL) {
                        consumer = streamCreateConsumer(group, name, rl->key, rl->db->id, SCC_DEFAULT);
                        if (noack) {
                            streamPropagateConsumerCreation(receiver, rl->key, receiver->bpop.xread_group, consumer->name);
                        }
                    }
                }

                /* Emit the two elements sub-array consisting of
                 * the name of the stream and the data we
                 * extracted from it. Wrapped in a single-item
                 * array, since we have just one key. */
                if (receiver->resp == 2) {
                    addReplyArrayLen(receiver, 1);
                    addReplyArrayLen(receiver, 2);
                }
                else {
                    addReplyMapLen(receiver, 1);
                }
                addReplyBulk(receiver, rl->key);

                streamPropInfo pi = {rl->key, receiver->bpop.xread_group};
                streamReplyWithRange(receiver, s, &start, NULL, receiver->bpop.xread_count, 0, group, consumer, noack, &pi);
                /* Note that after we unblock the client, 'gt'
                 * and other receiver->bpop stuff are no longer
                 * valid, so we must do the setup above before
                 * the unblockClient call. */

            unblock_receiver:
                updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
                unblockClient(receiver);
                afterCommand(receiver);
                server.current_client = old_client;
            }
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * in order to check if we can serve clients blocked by modules using
 * RM_BlockClientOnKeys(), when the corresponding key was signaled as ready:
 * our goal here is to call the RedisModuleBlockedClient reply() callback to
 * see if the key is really able to serve the client, and in that case,
 * unblock it. */
void serveClientsBlockedOnKeyByModule(readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_MODULE,
     * we can skip this loop. */
    if (!server.blocked_clients_by_type[BLOCKED_MODULE])
        return;

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    dictEntry *de = dictFind(rl->db->blocking_keys, rl->key);
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients, &li);

        while ((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_MODULE)
                continue;

            /* Note that if *this* client cannot be served by this key,
             * it does not mean that another client that is next into the
             * list cannot be served as well: they may be blocked by
             * different modules with different triggers to consider if a key
             * is ready or not. This means we can't exit the loop but need
             * to continue after the first failure. */
            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            if (!moduleTryServeClientBlockedOnKey(receiver, rl->key))
                continue;
            updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
            moduleUnblockClient(receiver);
            afterCommand(receiver);
            server.current_client = old_client;
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * when there may be clients blocked, via XREADGROUP, on an existing stream which
 * was deleted. We need to unblock the clients in that case.
 * The idea is that a client that is blocked via XREADGROUP is different from
 * any other blocking type in the sense that it depends on the existence of both
 * the key and the group. Even if the key is deleted and then revived with XADD
 * it won't help any clients blocked on XREADGROUP because the group no longer
 * exist, so they would fail with -NOGROUP anyway.
 * The conclusion is that it's better to unblock these client (with error) upon
 * the deletion of the key, rather than waiting for the first XADD. */
void unblockDeletedStreamReadgroupClients(readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_STREAM,
     * we can skip this loop. */
    if (!server.blocked_clients_by_type[BLOCKED_STREAM])
        return;

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    dictEntry *de = dictFind(rl->db->blocking_keys, rl->key);
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients, &li);

        while ((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_STREAM || !receiver->bpop.xread_group)
                continue;

            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            addReplyError(receiver, "-UNBLOCKED the stream key no longer exists");
            updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
            unblockClient(receiver);
            afterCommand(receiver);
            server.current_client = old_client;
        }
    }
}

/*
这个函数会在 Redis 每次执行完单个命令、事务块或 Lua 脚本之后调用.

 * All the keys with at least one client blocked that received at least
 * one new element via some write operation are accumulated into
 * the server.ready_keys list. This function will run the list and will
 * serve clients accordingly. Note that the function will iterate again and
 * again as a result of serving BLMOVE we can have new blocking clients
 * to serve because of the PUSH side of BLMOVE.
 *
 * 对所有被阻塞在某个客户端的 key 来说,只要这个 key 被执行了某种 PUSH 操作
 * 那么这个 key 就会被放到 serve.ready_keys 去.
 *
 * 这个函数会遍历整个 serve.ready_keys 链表,
 * 并将里面的 key 的元素弹出给被阻塞客户端,
 * 从而解除客户端的阻塞状态.
 *
 * 函数会一次又一次地进行迭代,
 * 因此它在执行 BRPOPLPUSH 命令的情况下也可以正常获取到正确的新被阻塞客户端.

 * This function is normally "fair", that is, it will server clients
 * using a FIFO behavior. However this fairness is violated in certain
 * edge cases, that is, when we have clients blocked at the same time
 * in a sorted set and in a list, for the same key (a very odd thing to
 * do client side, indeed!). Because mismatching clients (blocking for
 * a different type compared to the current key type) are moved in the
 * other side of the linked list. However as long as the key starts to
 * be used only for a single type, like virtually any Redis application will
 * do, the function is already fair.
 * */
void handleClientsBlockedOnKeys(void) {
    /* This function is called only when also_propagate is in its basic state
     * (i.e. not from call(), module context, etc.) */
    serverAssert(server.also_propagate.numops == 0);
    server.core_propagates = 1;

    while (listLength(server.ready_keys) != 0) {
        list *l;

        /* Point server.ready_keys to a fresh list and save the current one
         * locally. This way as we run the old list we are free to call
         * signalKeyAsReady() that may push new elements in server.ready_keys
         * when handling clients blocked into BLMOVE. */
        l = server.ready_keys;
        server.ready_keys = listCreate();

        while (listLength(l) != 0) {
            listNode *ln = listFirst(l);
            readyList *rl = ln->value;

            /* First of all remove this key from db->ready_keys so that
             * we can safely call signalKeyAsReady() against this key. */
            dictDelete(rl->db->ready_keys, rl->key);

            /* Even if we are not inside call(), increment the call depth
             * in order to make sure that keys are expired against a fixed
             * reference time, and not against the wallclock time. This
             * way we can lookup an object multiple times (BLMOVE does
             * that) without the risk of it being freed in the second
             * lookup, invalidating the first one.
             * See https://github.com/redis/redis/pull/6554. */
            server.fixed_time_expire++;
            updateCachedTime(0);

            /* Serve clients blocked on the key. */
            robj *o = lookupKeyReadWithFlags(rl->db, rl->key, LOOKUP_NONOTIFY | LOOKUP_NOSTATS);
            if (o != NULL) {
                int objtype = o->type;
                if (objtype == OBJ_LIST)
                    serveClientsBlockedOnListKey(o, rl);
                else if (objtype == OBJ_ZSET)
                    serveClientsBlockedOnSortedSetKey(o, rl);
                else if (objtype == OBJ_STREAM)
                    serveClientsBlockedOnStreamKey(o, rl);
                /* We want to serve clients blocked on module keys
                 * regardless of the object type: we don't know what the
                 * module is trying to accomplish right now. */
                serveClientsBlockedOnKeyByModule(rl);
                /* If we have XREADGROUP clients blocked on this key, and
                 * the key is not a stream, it must mean that the key was
                 * overwritten by either SET or something like
                 * (MULTI, DEL key, SADD key e, EXEC).
                 * In this case we need to unblock all these clients. */
                if (objtype != OBJ_STREAM)
                    unblockDeletedStreamReadgroupClients(rl);
            }
            else {
                /* Unblock all XREADGROUP clients of this deleted key */
                unblockDeletedStreamReadgroupClients(rl);
                /* Edge case: If lookupKeyReadWithFlags decides to expire the key we have to
                 * take care of the propagation here, because afterCommand wasn't called */
                if (server.also_propagate.numops > 0)
                    propagatePendingCommands();
            }
            server.fixed_time_expire--;

            /* Free this item. */
            decrRefCount(rl->key);
            zfree(rl);
            listDelNode(l, ln);
        }
        listRelease(l); /* We have the new list on place at this point. */
    }

    serverAssert(server.core_propagates); /* This function should not be re-entrant */

    server.core_propagates = 0;
}

/* This is how the current blocking lists/sorted sets/streams work, we use
 * BLPOP as example, but the concept is the same for other list ops, sorted
 * sets and XREAD.
 * - If the user calls BLPOP and the key exists and contains a non empty list
 *   then LPOP is called instead. So BLPOP is semantically the same as LPOP
 *   if blocking is not required.
 * - If instead BLPOP is called and the key does not exists or the list is
 *   empty we need to block. In order to do so we remove the notification for
 *   new data to read in the client socket (so that we'll not serve new
 *   requests if the blocking request is not served). Also we put the client
 *   in a dictionary (db->blocking_keys) mapping keys to a list of clients
 *   blocking for this keys.
 * - If a PUSH operation against a key with blocked clients waiting is
 *   performed, we mark this key as "ready", and after the current command,
 *   MULTI/EXEC block, or script, is executed, we serve all the clients waiting
 *   for this list, from the one that blocked first, to the last, accordingly
 *   to the number of elements we have in the ready list.
 */

// 设置客户端为指定键的阻塞模式(list,zset或stream),
// 使用指定的超时时间.'type'参数是BLOCKED_LIST,BLOCKED_ZSET或BLOCKED_STREAM
//*等待一个空键来唤醒客户端.客户端被阻塞为所有的'numkeys'键在'keys'参数.当我们为
//*流键,我们还提供了一组streamID结构:客户端将仅当项目的ID大于或等于指定时才被取消阻塞
//* 1被追加到流中.'count'用于支持可选count参数的命令.
//*其他值为0.* /
void blockForKeys(client *c, int btype, robj **keys, int numkeys, long count, mstime_t timeout, robj *target, struct blockPos *blockpos, streamID *ids) {
    dictEntry *de;
    list *l;
    int j;

    c->bpop.count = count;
    c->bpop.timeout = timeout;
    c->bpop.target = target;

    if (blockpos != NULL)
        c->bpop.blockpos = *blockpos;

    if (target != NULL)
        incrRefCount(target);

    for (j = 0; j < numkeys; j++) {
        /* 分配我们的bkinfo结构,关联到客户端阻塞的每个键.*/
        bkinfo *bki = zmalloc(sizeof(*bki));
        if (btype == BLOCKED_STREAM)
            bki->stream_id = ids[j];

        /* 如果键已经存在于字典中,忽略它.*/
        if (dictAdd(c->bpop.keys, keys[j], bki) != DICT_OK) {
            zfree(bki);
            continue;
        }
        incrRefCount(keys[j]);

        // 在另一侧   keys->clients 的映射
        de = dictFind(c->db->blocking_keys, keys[j]);
        if (de == NULL) {
            int retval;

            // 对于每个key,我们获取一个为它阻塞的客户端列表
            l = listCreate();
            retval = dictAdd(c->db->blocking_keys, keys[j], l);
            incrRefCount(keys[j]);
            serverAssertWithInfo(c, keys[j], retval == DICT_OK);
        }
        else {
            l = dictGetVal(de);
        }
        listAddNodeTail(l, c);
        bki->listnode = listLast(l);
    }
    blockClient(c, btype);
}

/* Unblock a client that's waiting in a blocking operation such as BLPOP.
 * You should never call this function directly, but unblockClient() instead. */
void unblockClientWaitingData(client *c) {
    dictEntry *de;
    dictIterator *di;
    list *l;

    serverAssertWithInfo(c, NULL, dictSize(c->bpop.keys) != 0);
    di = dictGetIterator(c->bpop.keys);
    /* The client may wait for multiple keys, so unblock it for every key. */
    while ((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        bkinfo *bki = dictGetVal(de);

        /* Remove this client from the list of clients waiting for this key. */
        l = dictFetchValue(c->db->blocking_keys, key);
        serverAssertWithInfo(c, key, l != NULL);
        listDelNode(l, bki->listnode);
        /* If the list is empty we need to remove it to avoid wasting memory */
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys, key);
    }
    dictReleaseIterator(di);

    /* Cleanup the client structure */
    dictEmpty(c->bpop.keys, NULL);
    if (c->bpop.target) {
        decrRefCount(c->bpop.target);
        c->bpop.target = NULL;
    }
    if (c->bpop.xread_group) {
        decrRefCount(c->bpop.xread_group);
        decrRefCount(c->bpop.xread_consumer);
        c->bpop.xread_group = NULL;
        c->bpop.xread_consumer = NULL;
    }
}

static int getBlockedTypeByType(int type) {
    switch (type) {
        case OBJ_LIST:
            return BLOCKED_LIST;
        case OBJ_ZSET:
            return BLOCKED_ZSET;
        case OBJ_MODULE:
            return BLOCKED_MODULE;
        case OBJ_STREAM:
            return BLOCKED_STREAM;
        default:
            return BLOCKED_NONE;
    }
}

/* If the specified key has clients blocked waiting for list pushes, this
 * function will put the key reference into the server.ready_keys list.
 * Note that db->ready_keys is a hash table that allows us to avoid putting
 * the same key again and again in the list in case of multiple pushes
 * made by a script or in the context of MULTI/EXEC.
 *
 * The list will be finally processed by handleClientsBlockedOnKeys() */
void signalKeyAsReady(redisDb *db, robj *key, int type) {
    readyList *rl;

    /* Quick returns. */
    int btype = getBlockedTypeByType(type);
    if (btype == BLOCKED_NONE) {
        /* The type can never block. */
        return;
    }
    if (!server.blocked_clients_by_type[btype] && !server.blocked_clients_by_type[BLOCKED_MODULE]) {
        /* No clients block on this type. Note: Blocked modules are represented
         * by BLOCKED_MODULE, even if the intention is to wake up by normal
         * types (list, zset, stream), so we need to check that there are no
         * blocked modules before we do a quick return here. */
        return;
    }

    /* No clients blocking for this key? No need to queue it. */
    if (dictFind(db->blocking_keys, key) == NULL)
        return;

    /* Key was already signaled? No need to queue it again. */
    if (dictFind(db->ready_keys, key) != NULL)
        return;

    /* Ok, we need to queue this key into server.ready_keys. */
    rl = zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = db;
    incrRefCount(key);
    listAddNodeTail(server.ready_keys, rl);

    /* We also add the key in the db->ready_keys dictionary in order
     * to avoid adding it multiple times into a list with a simple O(1)
     * check. */
    incrRefCount(key);
    serverAssert(dictAdd(db->ready_keys, key, NULL) == DICT_OK);
}
