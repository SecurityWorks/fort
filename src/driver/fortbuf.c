/* Fort Firewall Log Buffer */

#include "fortbuf.h"

#include "fortdbg.h"
#include "fortdev.h"
#include "forttrace.h"
#include "fortutl.h"

#define FORT_BUFFER_POOL_TAG 'BwfF'

static FORT_APP_PATH fort_buffer_adjust_log_path(PCFORT_CONF_META_CONN conn)
{
    FORT_APP_PATH log_path = conn->real_path;

    if (log_path.len > FORT_LOG_PATH_MAX) {
        log_path.len = 0; /* drop too long path */
    }

    return log_path;
}

static PFORT_BUFFER_DATA fort_buffer_data_new(PFORT_BUFFER buf)
{
    PFORT_BUFFER_DATA data = buf->data_free;

    if (data != NULL) {
        buf->data_free = data->next;
    } else {
        data = fort_mem_alloc(sizeof(FORT_BUFFER_DATA), FORT_BUFFER_POOL_TAG);
    }

    return data;
}

static void fort_buffer_data_del(PFORT_BUFFER_DATA data)
{
    while (data != NULL) {
        PFORT_BUFFER_DATA next = data->next;
        fort_mem_free(data, FORT_BUFFER_POOL_TAG);
        data = next;
    }
}

static PFORT_BUFFER_DATA fort_buffer_data_alloc(PFORT_BUFFER buf, UINT32 len)
{
    PFORT_BUFFER_DATA data = buf->data_tail;

    if (data == NULL || len > FORT_BUFFER_SIZE - data->top) {
        if (len > FORT_BUFFER_SIZE)
            return NULL;

        PFORT_BUFFER_DATA new_data = fort_buffer_data_new(buf);
        if (new_data == NULL)
            return NULL;

        new_data->top = 0;
        new_data->next = NULL;

        if (data == NULL) {
            buf->data_head = new_data;
        } else {
            data->next = new_data;
        }

        buf->data_tail = new_data;

        data = new_data;
    }

    return data;
}

static void fort_buffer_data_shift(PFORT_BUFFER buf)
{
    PFORT_BUFFER_DATA data = buf->data_head;

    buf->data_head = data->next;

    if (data->next == NULL) {
        buf->data_tail = NULL;
    }

    data->next = buf->data_free;
    buf->data_free = data;
}

FORT_API void fort_buffer_open(PFORT_BUFFER buf)
{
    KeInitializeSpinLock(&buf->lock);
}

FORT_API void fort_buffer_close(PFORT_BUFFER buf)
{
    fort_buffer_data_del(buf->data_head);
    fort_buffer_data_del(buf->data_free);
}

FORT_API void fort_buffer_clear(PFORT_BUFFER buf)
{
    KLOCK_QUEUE_HANDLE lock_queue;
    KeAcquireInStackQueuedSpinLock(&buf->lock, &lock_queue);

    fort_buffer_close(buf);

    buf->data_head = NULL;
    buf->data_tail = NULL;
    buf->data_free = NULL;

    KeReleaseInStackQueuedSpinLock(&lock_queue);
}

inline static NTSTATUS fort_buffer_prepare_pending(
        PFORT_BUFFER buf, UINT32 len, PCHAR *out, PIRP *irp, ULONG_PTR *info)
{
    const UINT32 out_top = buf->out_top;
    UINT32 new_top = out_top + len;

    /* Is it time to flush logs? */
    if (buf->out_len - new_top < FORT_LOG_SIZE_MAX) {
        if (irp != NULL && *irp == NULL) {
            *irp = buf->irp;
            buf->irp = NULL;

            *info = new_top;
            new_top = 0;
        }

        buf->out_len = new_top;
    }

    *out = buf->out + out_top;
    buf->out_top = new_top;

    return STATUS_SUCCESS;
}

inline static NTSTATUS fort_buffer_prepare_new(PFORT_BUFFER buf, UINT32 len, PCHAR *out)
{
    PFORT_BUFFER_DATA data = fort_buffer_data_alloc(buf, len);
    if (data == NULL) {
        LOG("Buffer OOM: len=%d\n", len);
        TRACE(FORT_BUFFER_OOM, STATUS_INSUFFICIENT_RESOURCES, len, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *out = data->p + data->top;
    data->top += len;

    return STATUS_SUCCESS;
}

FORT_API NTSTATUS fort_buffer_prepare(
        PFORT_BUFFER buf, UINT32 len, PCHAR *out, PIRP *irp, ULONG_PTR *info)
{
    /* Check a pending buffer */
    if (buf->data_head == NULL) {
        const ULONG out_len = buf->out_len;

        if (out_len != 0 && buf->out_top < out_len) {
            return fort_buffer_prepare_pending(buf, len, out, irp, info);
        }
    }

    return fort_buffer_prepare_new(buf, len, out);
}

FORT_API NTSTATUS fort_buffer_conn_write(PFORT_BUFFER buf, PCFORT_CONF_META_CONN conn,
        FORT_BUFFER_CONN_WRITE_TYPE log_type, PIRP *irp, ULONG_PTR *info)
{
    NTSTATUS status;

    const FORT_APP_PATH log_path = fort_buffer_adjust_log_path(conn);

    UINT32 len;
    switch (log_type) {
    case FORT_BUFFER_CONN_WRITE_BLOCKED: {
        len = FORT_LOG_BLOCKED_SIZE(log_path.len);
    } break;
    case FORT_BUFFER_CONN_WRITE_BLOCKED_IP: {
        len = FORT_LOG_BLOCKED_IP_SIZE(log_path.len, conn->isIPv6);
    } break;
    case FORT_BUFFER_CONN_WRITE_PROC_NEW: {
        len = FORT_LOG_PROC_NEW_SIZE(log_path.len);
    } break;
    }

    KLOCK_QUEUE_HANDLE lock_queue;
    KeAcquireInStackQueuedSpinLock(&buf->lock, &lock_queue);
    {
        PCHAR out;
        status = fort_buffer_prepare(buf, len, &out, irp, info);

        if (NT_SUCCESS(status)) {
            switch (log_type) {
            case FORT_BUFFER_CONN_WRITE_BLOCKED: {
                fort_log_blocked_write(out, conn->blocked, conn->process_id, &log_path);
            } break;
            case FORT_BUFFER_CONN_WRITE_BLOCKED_IP: {
                fort_log_blocked_ip_write(out, conn, &log_path);
            } break;
            case FORT_BUFFER_CONN_WRITE_PROC_NEW: {
                fort_log_proc_new_write(out, conn->process_id, &log_path);
            } break;
            }
        }
    }
    KeReleaseInStackQueuedSpinLock(&lock_queue);

    return status;
}

inline static NTSTATUS fort_buffer_xmove_locked_empty(
        PFORT_BUFFER buf, PIRP irp, PVOID out, ULONG out_len)
{
    if (buf->out_len != 0)
        return STATUS_UNSUCCESSFUL; /* collision */

    buf->irp = irp;
    buf->out = out;
    buf->out_len = out_len;
    buf->out_top = 0;

    return STATUS_PENDING;
}

static NTSTATUS fort_buffer_xmove_locked(
        PFORT_BUFFER buf, PIRP irp, PVOID out, ULONG out_len, ULONG_PTR *info)
{
    PFORT_BUFFER_DATA data = buf->data_head;
    const UINT32 buf_top = (data ? data->top : 0);

    *info = buf_top;

    if (buf_top == 0)
        return fort_buffer_xmove_locked_empty(buf, irp, out, out_len);

    if (out_len < buf_top)
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(out, data->p, buf_top);

    fort_buffer_data_shift(buf);

    return STATUS_SUCCESS;
}

FORT_API NTSTATUS fort_buffer_xmove(
        PFORT_BUFFER buf, PIRP irp, PVOID out, ULONG out_len, ULONG_PTR *info)
{
    KLOCK_QUEUE_HANDLE lock_queue;
    KeAcquireInStackQueuedSpinLock(&buf->lock, &lock_queue);

    const NTSTATUS status = fort_buffer_xmove_locked(buf, irp, out, out_len, info);

    KeReleaseInStackQueuedSpinLock(&lock_queue);

    return status;
}

inline static NTSTATUS fort_buffer_cancel_pending(PFORT_BUFFER buf, PIRP irp, ULONG_PTR *info)
{
    NTSTATUS status = STATUS_NOT_FOUND;

    *info = 0;

    /* Cancel routines are called at IRQL = DISPATCH_LEVEL */

    KLOCK_QUEUE_HANDLE lock_queue;
    KeAcquireInStackQueuedSpinLockAtDpcLevel(&buf->lock, &lock_queue);
    if (irp == buf->irp) {
        buf->irp = NULL;
        buf->out_len = 0;

        status = STATUS_CANCELLED;

        if (buf->out_top != 0) {
            *info = buf->out_top;
            buf->out_top = 0;

            status = STATUS_SUCCESS;
        }
    }
    KeReleaseInStackQueuedSpinLockFromDpcLevel(&lock_queue);

    return status;
}

static void fort_device_cancel_pending(PDEVICE_OBJECT device, PIRP irp)
{
    UNUSED(device);

    /* Already called: IoAcquireCancelSpinLock(irp->CancelIrql); */

    FORT_CHECK_STACK(FORT_DEVICE_CANCEL_PENDING);

    ULONG_PTR info;

    const NTSTATUS status = fort_buffer_cancel_pending(&fort_device()->buffer, irp, &info);

    IoSetCancelRoutine(irp, NULL);
    IoReleaseCancelSpinLock(irp->CancelIrql); /* before IoCompleteRequest()! */

    if (status != STATUS_NOT_FOUND) {
        fort_request_complete_info(irp, status, info);
    }
}

FORT_API void fort_buffer_irp_mark_pending(PIRP irp)
{
    IoMarkIrpPending(irp);

    fort_irp_set_cancel_routine(irp, &fort_device_cancel_pending);
}

FORT_API void fort_buffer_irp_clear_pending(PIRP irp)
{
    fort_irp_set_cancel_routine(irp, NULL);
}

FORT_API void fort_buffer_dpc_begin(PFORT_BUFFER buf, PKLOCK_QUEUE_HANDLE lock_queue)
{
    KeAcquireInStackQueuedSpinLockAtDpcLevel(&buf->lock, lock_queue);
}

FORT_API void fort_buffer_dpc_end(PKLOCK_QUEUE_HANDLE lock_queue)
{
    KeReleaseInStackQueuedSpinLockFromDpcLevel(lock_queue);
}

FORT_API void fort_buffer_flush_pending(PFORT_BUFFER buf, PIRP *irp, ULONG_PTR *info)
{
    UINT32 out_top = buf->out_top;

    /* Move data from buffer to pending */
    if (out_top == 0 && buf->out_len != 0) {
        PFORT_BUFFER_DATA data = buf->data_head;

        out_top = (data ? data->top : 0);

        if (out_top != 0) {
            RtlCopyMemory(buf->out, data->p, out_top);

            fort_buffer_data_shift(buf);
        }
    }

    if (out_top != 0) {
        *info = out_top;

        buf->out_top = 0;
        buf->out_len = 0;

        *irp = buf->irp;
        buf->irp = NULL;
    }
}
