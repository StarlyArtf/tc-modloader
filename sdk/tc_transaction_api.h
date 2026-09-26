#ifndef TC_TRANSACTION_API_H
#define TC_TRANSACTION_API_H
#include <stdint.h>
#include "tc_command_api.h"

#define TC_SERVICE_TRANSACTIONS "tc.transactions"
#define TC_TRANSACTION_API_VERSION_1 1u
#define TC_TRANSACTION_API_VERSION_2 2u
#define TC_TRANSACTION_STATUS_VERSION_1 1u

#define TC_TRANSACTION_SAVE_ON_COMMIT (1u << 0)

#define TC_TRANSACTION_STATE_OPEN 1u
#define TC_TRANSACTION_STATE_QUEUED 2u
#define TC_TRANSACTION_STATE_RUNNING 3u
#define TC_TRANSACTION_STATE_COMMITTED 4u
#define TC_TRANSACTION_STATE_FAILED 5u
#define TC_TRANSACTION_STATE_ABORTED 6u
#define TC_TRANSACTION_STATE_CONFLICT 7u

#define TC_TRANSACTION_OK 0
#define TC_TRANSACTION_ERR_UNAVAILABLE (-1)
#define TC_TRANSACTION_ERR_ARGUMENT (-2)
#define TC_TRANSACTION_ERR_THREAD (-3)
#define TC_TRANSACTION_ERR_CAPACITY (-4)
#define TC_TRANSACTION_ERR_NOT_FOUND (-5)
#define TC_TRANSACTION_ERR_STATE (-6)
#define TC_TRANSACTION_ERR_STALE (-7)
#define TC_TRANSACTION_ERR_CONFLICT (-8)

typedef struct TCTransactionStatusV1 {
    uint32_t size;
    uint32_t version;
    uint32_t state;
    int32_t result;
    uint64_t transaction_id;
    uint64_t staged_count;
    uint64_t completed_count;
    int64_t submitted_frame;
    int64_t completed_frame;
} TCTransactionStatusV1;

typedef struct TCTransactionApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*begin)(void* context,const TCGameHandle* board,uint32_t flags,uint64_t* transaction_id);
    int (*stage)(void* context,uint64_t transaction_id,const TCCommandV1* command);
    int (*commit)(void* context,uint64_t transaction_id);
    int (*abort)(void* context,uint64_t transaction_id);
    int (*get_status)(void* context,uint64_t transaction_id,TCTransactionStatusV1* out,uint32_t out_size);
} TCTransactionApiV1;

/* V2 stages the richer command form, so a staged step can carry a placement
   payload.  The prefix is the V1 table; V1 keeps working unchanged. */
typedef struct TCTransactionApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*begin)(void* context,const TCGameHandle* board,uint32_t flags,uint64_t* transaction_id);
    int (*stage)(void* context,uint64_t transaction_id,const TCCommandV2* command);
    int (*commit)(void* context,uint64_t transaction_id);
    int (*abort)(void* context,uint64_t transaction_id);
    int (*get_status)(void* context,uint64_t transaction_id,TCTransactionStatusV1* out,uint32_t out_size);
} TCTransactionApiV2;

#endif
