#include "over-server.h"
#include <unistd.h>
#include <fcntl.h>

typedef struct {
    size_t keys;
    size_t cow;
    monotime cow_updated;
    double progress;
    childInfoType information_type; // 子进程类型
} child_info_data;

// 打开当前进程和子进程之间的进程通信
void openChildInfoPipe(void) {
    if (anetPipe(server.child_info_pipe, O_NONBLOCK, 0) == -1) {
        /* On error our two file descriptors should be still set to -1,
         * but we call anyway closeChildInfoPipe() since can't hurt. */
        closeChildInfoPipe();
    }
    else {
        server.child_info_nread = 0;
    }
}

void closeChildInfoPipe(void) {
    if (server.child_info_pipe[0] != -1 || server.child_info_pipe[1] != -1) {
        //关闭管道
        close(server.child_info_pipe[0]);
        close(server.child_info_pipe[1]);
        //并将值置为-1
        server.child_info_pipe[0] = -1;
        server.child_info_pipe[1] = -1;
        server.child_info_nread = 0;
    }
}

// 发送数据到父进程
void sendChildInfoGeneric(childInfoType info_type, size_t keys, double progress, char *pname) {
    if (server.child_info_pipe[1] == -1)
        return;

    static monotime cow_updated = 0;
    static uint64_t cow_update_cost = 0;
    static size_t cow = 0;
    static size_t peak_cow = 0;
    static size_t update_count = 0;
    static unsigned long long sum_cow = 0;

    child_info_data data = {0}; /* zero everything, including padding to satisfy valgrind */

    /* When called to report current info, we need to throttle down CoW updates as they
     * can be very expensive. To do that, we measure the time it takes to get a reading
     * and schedule the next reading to happen not before time*CHILD_COW_COST_FACTOR
     * passes. */

    monotime now = getMonotonicUs();
    if (info_type != CHILD_INFO_TYPE_CURRENT_INFO || !cow_updated || now - cow_updated > cow_update_cost * CHILD_COW_DUTY_CYCLE) {
        cow = zmalloc_get_private_dirty(-1);
        cow_updated = getMonotonicUs();
        cow_update_cost = cow_updated - now;
        if (cow > peak_cow)
            peak_cow = cow;
        sum_cow += cow;
        update_count++;

        int cow_info = (info_type != CHILD_INFO_TYPE_CURRENT_INFO);
        if (cow || cow_info) {
            serverLog(cow_info ? LL_NOTICE : LL_VERBOSE, "Fork CoW for %s: current %zu MB, peak %zu MB, average %llu MB", pname, cow >> 20, peak_cow >> 20, (sum_cow / update_count) >> 20);
        }
    }

    data.information_type = info_type;
    data.keys = keys;
    data.cow = cow;
    data.cow_updated = cow_updated;
    data.progress = progress;

    ssize_t wlen = sizeof(data);

    if (write(server.child_info_pipe[1], &data, wlen) != wlen) { /* Nothing to do on error, this will be detected by the other side. */
    }
}

void updateChildInfo(childInfoType information_type, size_t cow, monotime cow_updated, size_t keys, double progress) {
    if (cow > server.stat_current_cow_peak)
        server.stat_current_cow_peak = cow;

    if (information_type == CHILD_INFO_TYPE_CURRENT_INFO) {
        server.stat_current_cow_bytes = cow;
        server.stat_current_cow_updated = cow_updated;
        server.stat_current_save_keys_processed = keys;
        if (progress != -1)
            server.stat_module_progress = progress;
    }
    else if (information_type == CHILD_INFO_TYPE_AOF_COW_SIZE) {
        server.stat_aof_cow_bytes = server.stat_current_cow_peak;
    }
    else if (information_type == CHILD_INFO_TYPE_RDB_COW_SIZE) {
        server.stat_rdb_cow_bytes = server.stat_current_cow_peak;
    }
    else if (information_type == CHILD_INFO_TYPE_MODULE_COW_SIZE) {
        server.stat_module_cow_bytes = server.stat_current_cow_peak;
    }
}

// 从管道中读取子进程数据。如果将完整的数据读入缓冲区，则将数据存储到 缓冲区，并返回1。
// 否则，部分数据将留在缓冲区中，等待下一次读取，并返回0。
int readChildInfo(childInfoType *information_type, size_t *cow, monotime *cow_updated, size_t *keys, double *progress) {
    /* We are using here a static buffer in combination with the server.child_info_nread to handle short reads */
    static child_info_data buffer;
    ssize_t wlen = sizeof(buffer);

    /* Do not overlap */
    if (server.child_info_nread == wlen)
        server.child_info_nread = 0;

    int nread = read(server.child_info_pipe[0], (char *)&buffer + server.child_info_nread, wlen - server.child_info_nread);
    if (nread > 0) {
        server.child_info_nread += nread;
    }

    /* We have complete child info */
    if (server.child_info_nread == wlen) {
        *information_type = buffer.information_type;
        *cow = buffer.cow;
        *cow_updated = buffer.cow_updated;
        *keys = buffer.keys;
        *progress = buffer.progress;
        return 1;
    }
    else {
        return 0;
    }
}

// 从子节点接收信息数据。
void receiveChildInfo(void) {
    if (server.child_info_pipe[0] == -1)
        return;

    size_t cow;
    monotime cow_updated;
    size_t keys;
    double progress;
    childInfoType information_type;

    // 释放管道并更新子信息，以便获得最终消息。
    while (readChildInfo(&information_type, &cow, &cow_updated, &keys, &progress)) {
        updateChildInfo(information_type, cow, cow_updated, keys, progress);
    }
}
