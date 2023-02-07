#ifndef SRC_CALL_REPLY_H_
#define SRC_CALL_REPLY_H_

#include "over-resp_parser.h"

typedef struct CallReply CallReply;

CallReply *callReplyCreate(sds reply, list *deferred_error_list, void *private_data);

CallReply *callReplyCreateError(sds reply, void *private_data);

int callReplyType(CallReply *rep);

const char *callReplyGetString(CallReply *rep, size_t *len);

long long callReplyGetLongLong(CallReply *rep);

double callReplyGetDouble(CallReply *rep);

int callReplyGetBool(CallReply *rep);

size_t callReplyGetLen(CallReply *rep);

CallReply *callReplyGetArrayElement(CallReply *rep, size_t idx);

CallReply *callReplyGetSetElement(CallReply *rep, size_t idx);

int callReplyGetMapElement(CallReply *rep, size_t idx, CallReply **key, CallReply **val);

CallReply *callReplyGetAttribute(CallReply *rep);

int callReplyGetAttributeElement(CallReply *rep, size_t idx, CallReply **key, CallReply **val);

const char *callReplyGetBigNumber(CallReply *rep, size_t *len);

const char *callReplyGetVerbatim(CallReply *rep, size_t *len, const char **format);

const char *callReplyGetProto(CallReply *rep, size_t *len);

void *callReplyGetPrivateData(CallReply *rep);

int callReplyIsResp3(CallReply *rep);

list *callReplyDeferredErrorList(CallReply *rep);

void freeCallReply(CallReply *rep);

#endif /* SRC_CALL_REPLY_H_ */
