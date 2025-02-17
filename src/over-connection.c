#include "over-server.h"
#include "over-connhelpers.h"

ConnectionType CT_Socket;

// 创建链接结构体,后续会将描述符塞到这个结构体里
connection *connCreateSocket() {
    connection *conn = zcalloc(sizeof(connection));
    conn->type = &CT_Socket;
    conn->fd = -1;
    return conn;
}

// 创建一个新的套接字类型连接,该连接已经与已接受的连接相关联.
// 直到调用了connAccept()并调用了连接级接受处理程序,套接字才为I/O做好准备.
// 调用者应该使用connGetState()并验证创建的连接是否处于错误状态(这对于套接字连接是不可能的,但对于其他协议是可能的).
connection *connCreateAcceptedSocket(int fd) {
    connection *conn = connCreateSocket();
    conn->fd = fd;
    conn->state = CONN_STATE_ACCEPTING; // 接收数据中
    return conn;
}

static int connSocketConnect(connection *conn, const char *addr, int port, const char *src_addr, ConnectionCallbackFunc connect_handler) {
    // 从库和主库建立链接
    int fd = anetTcpNonBlockBestEffortBindConnect(NULL, addr, port, src_addr);
    if (fd == -1) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = errno;
        return C_ERR;
    }

    conn->fd = fd;
    conn->state = CONN_STATE_CONNECTING;

    conn->conn_handler = connect_handler;
    // 在建立的链接上注册写事件,对应的回调函数是  syncWithMaster
    aeCreateFileEvent(server.el, conn->fd, AE_WRITABLE, conn->type->ae_handler, conn);

    return C_OK;
}

int connHasWriteHandler(connection *conn) {
    return conn->write_handler != NULL;
}

int connHasReadHandler(connection *conn) {
    return conn->read_handler != NULL;
}

/* 将私有数据指针与连接关联 */
void connSetPrivateData(connection *conn, void *data) {
    conn->private_data = data;
}

void *connGetPrivateData(connection *conn) {
    return conn->private_data;
}

static void connSocketClose(connection *conn) {
    if (conn->fd != -1) {
        aeDeleteFileEvent(server.el, conn->fd, AE_READABLE | AE_WRITABLE);
        close(conn->fd);
        conn->fd = -1;
    }

    if (connHasRefs(conn)) {
        conn->flags |= CONN_FLAG_CLOSE_SCHEDULED;
        return;
    }

    zfree(conn);
}

static int connSocketWrite(connection *conn, const void *data, size_t data_len) {
    int ret = write(conn->fd, data, data_len);
    if (ret < 0 && errno != EAGAIN) {
        conn->last_errno = errno;
        if (errno != EINTR && conn->state == CONN_STATE_CONNECTED)
            conn->state = CONN_STATE_ERROR;
    }

    return ret;
}

static int connSocketWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    int ret = writev(conn->fd, iov, iovcnt);
    if (ret < 0 && errno != EAGAIN) {
        conn->last_errno = errno;
        if (errno != EINTR && conn->state == CONN_STATE_CONNECTED)
            conn->state = CONN_STATE_ERROR;
    }

    return ret;
}

static int connSocketRead(connection *conn, void *buf, size_t buf_len) {
    int ret = read(conn->fd, buf, buf_len);
    if (!ret) {
        conn->state = CONN_STATE_CLOSED;
    }
    else if (ret < 0 && errno != EAGAIN) {
        conn->last_errno = errno;

        if (errno != EINTR && conn->state == CONN_STATE_CONNECTED)
            conn->state = CONN_STATE_ERROR;
    }

    return ret;
}

// OK
static int connSocketAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    int ret = C_OK;

    if (conn->state != CONN_STATE_ACCEPTING)
        return C_ERR;
    conn->state = CONN_STATE_CONNECTED;

    connIncrRefs(conn);
    if (!callHandler(conn, accept_handler)) {
        ret = C_ERR;
    }
    connDecrRefs(conn);

    return ret;
}

// 注册一个写处理程序,在连接可读时调用.如果为NULL,则删除现有的处理程序.
static int connSocketSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {
    if (func == conn->write_handler)
        return C_OK;

    conn->write_handler = func;
    if (barrier)
        conn->flags |= CONN_FLAG_WRITE_BARRIER;
    else
        conn->flags &= ~CONN_FLAG_WRITE_BARRIER;
    if (!conn->write_handler)
        aeDeleteFileEvent(server.el, conn->fd, AE_WRITABLE);
    else if (aeCreateFileEvent(server.el, conn->fd, AE_WRITABLE, conn->type->ae_handler, conn) == AE_ERR) // 创建可写事件的监听,以及设置回调函数
        return C_ERR;
    return C_OK;
}

// 注册一个读处理程序,在连接可读时调用.如果为NULL,则删除现有的处理程序.
static int connSocketSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    if (func == conn->read_handler)
        return C_OK;

    conn->read_handler = func;
    if (!conn->read_handler)
        aeDeleteFileEvent(server.el, conn->fd, AE_READABLE);
    else if (aeCreateFileEvent(server.el, conn->fd, AE_READABLE, conn->type->ae_handler, conn) == AE_ERR)
        return C_ERR;
    return C_OK;
}

static const char *connSocketGetLastError(connection *conn) {
    return strerror(conn->last_errno);
}

// 客户端链接的可读事件回调
static void connSocketEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
    UNUSED(el);
    UNUSED(fd);
    connection *conn = clientData;

    if (conn->state == CONN_STATE_CONNECTING && (mask & AE_WRITABLE) && conn->conn_handler) {
        int conn_error = connGetSocketError(conn);
        if (conn_error) {
            conn->last_errno = conn_error;
            conn->state = CONN_STATE_ERROR;
        }
        else {
            conn->state = CONN_STATE_CONNECTED;
        }

        if (!conn->write_handler) {
            aeDeleteFileEvent(server.el, conn->fd, AE_WRITABLE);
        }

        if (!callHandler(conn, conn->conn_handler)) {
            return;
        }
        conn->conn_handler = NULL;
    }

    // 通常我们先执行可读事件,然后执行可写事件.这很有用,因为有时我们可以在处理完查询后立即回复查询.
    // 然而,如果在掩码中设置了WRITE_BARRIER,我们的应用程序会要求我们做相反的事情:
    // 永远不要在可读事件之后触发可写事件.在这种情况下,我们反转调用.
    // 这是有用的,例如,我们想要在beforeSleep()钩子中做一些事情,比如在回复客户端之前将文件同步到磁盘.
    int invert = conn->flags & CONN_FLAG_WRITE_BARRIER;

    int call_write = (mask & AE_WRITABLE) && conn->write_handler;
    int call_read = (mask & AE_READABLE) && conn->read_handler;

    // 处理正常的I/O流
    if (!invert && call_read) {
        if (!callHandler(conn, conn->read_handler))
            return;
    }
    // 触发可写事件
    if (call_write) {
        if (!callHandler(conn, conn->write_handler))
            return;
    }
    // 如果必须反转调用,则在可写事件之后触发可读事件.
    if (invert && call_read) {
        if (!callHandler(conn, conn->read_handler))
            return;
    }
}

static int connSocketBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {
    int fd = anetTcpNonBlockConnect(NULL, addr, port);
    if (fd == -1) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = errno;
        return C_ERR;
    }

    if ((aeWait(fd, AE_WRITABLE, timeout) & AE_WRITABLE) == 0) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = ETIMEDOUT;
    }

    conn->fd = fd;
    conn->state = CONN_STATE_CONNECTED;
    return C_OK;
}

/* Connection-based versions of syncio.c functions.
 * NOTE: This should ideally be refactored out in favor of pure async work.
 */

static ssize_t connSocketSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncWrite(conn->fd, ptr, size, timeout);
}

static ssize_t connSocketSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncRead(conn->fd, ptr, size, timeout);
}

static ssize_t connSocketSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncReadLine(conn->fd, ptr, size, timeout);
}

static int connSocketGetType(connection *conn) {
    (void)conn;

    return CONN_TYPE_SOCKET;
}

ConnectionType CT_Socket = {
    .ae_handler = connSocketEventHandler, // client可读时触发,会调用 .connect  .read
    .close = connSocketClose,
    .write = connSocketWrite,
    .writev = connSocketWritev,
    .read = connSocketRead,
    .accept = connSocketAccept,
    .connect = connSocketConnect,
    .set_write_handler = connSocketSetWriteHandler, // conn->write_handler=
    .set_read_handler = connSocketSetReadHandler,   // conn->read_handler=readQueryFromClient
    .get_last_error = connSocketGetLastError,
    .blocking_connect = connSocketBlockingConnect,
    .sync_write = connSocketSyncWrite,
    .sync_read = connSocketSyncRead,
    .sync_readline = connSocketSyncReadLine,
    .get_type = connSocketGetType};

int connGetSocketError(connection *conn) {
    int sockerr = 0;
    socklen_t errlen = sizeof(sockerr);

    if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1)
        sockerr = errno;
    return sockerr;
}

// 返回对端IP
int connPeerToString(connection *conn, char *ip, size_t ip_len, int *port) {
    return anetFdToString(conn ? conn->fd : -1, ip, ip_len, port, FD_TO_PEER_NAME);
}

int connSockName(connection *conn, char *ip, size_t ip_len, int *port) {
    return anetFdToString(conn->fd, ip, ip_len, port, FD_TO_SOCK_NAME);
}

int connFormatFdAddr(connection *conn, char *buf, size_t buf_len, int fd_to_str_type) {
    return anetFormatFdAddr(conn ? conn->fd : -1, buf, buf_len, fd_to_str_type);
}

int connBlock(connection *conn) {
    if (conn->fd == -1)
        return C_ERR;
    return anetBlock(NULL, conn->fd);
}

// 将连接设为非阻塞模式
int connNonBlock(connection *conn) {
    if (conn->fd == -1)
        return C_ERR;
    return anetNonBlock(NULL, conn->fd);
}

int connEnableTcpNoDelay(connection *conn) {
    // https://blog.csdn.net/qq_32907195/article/details/120287099
    if (conn->fd == -1)
        return C_ERR;
    return anetEnableTcpNoDelay(NULL, conn->fd);
}

int connDisableTcpNoDelay(connection *conn) {
    if (conn->fd == -1)
        return C_ERR;
    return anetDisableTcpNoDelay(NULL, conn->fd);
}

int connKeepAlive(connection *conn, int interval) {
    if (conn->fd == -1)
        return C_ERR;
    return anetKeepAlive(NULL, conn->fd, interval);
}

int connSendTimeout(connection *conn, long long ms) {
    return anetSendTimeout(NULL, conn->fd, ms);
}

int connRecvTimeout(connection *conn, long long ms) {
    return anetRecvTimeout(NULL, conn->fd, ms);
}

int connGetState(connection *conn) {
    return conn->state;
}

// 返回描述连接的文本
const char *connGetInfo(connection *conn, char *buf, size_t buf_len) {
    snprintf(buf, buf_len - 1, "fd=%i", conn == NULL ? -1 : conn->fd);
    return buf;
}
