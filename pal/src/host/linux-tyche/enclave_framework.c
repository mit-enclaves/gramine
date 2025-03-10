#include <stdbool.h>

#include "api.h"
#include "crypto.h"
#include "enclave_tf.h"
#include "hex.h"
#include "list.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_linux.h"
#include "pal_linux_error.h"
#include "path_utils.h"
#include "sgx_arch.h"
#include "spinlock.h"
#include "toml.h"
#include "toml_utils.h"

#define LOCAL_ATTESTATION_TAG_PARENT_STR "GRAMINE_LOCAL_ATTESTATION_TAG_PARENT"
#define LOCAL_ATTESTATION_TAG_CHILD_STR "GRAMINE_LOCAL_ATTESTATION_TAG_CHILD"

static int register_file(const char* uri, const char* hash_str, bool check_duplicates);

uintptr_t g_enclave_base;
uintptr_t g_enclave_top;
bool g_allowed_files_warn = false;

/*
 * SGX's EGETKEY(SEAL_KEY) uses three masks as key-derivation material:
 *   - KEYREQUEST.ATTRIBUTESMASK.FLAGS
 *   - KEYREQUEST.ATTRIBUTESMASK.XFRM
 *   - KEYREQUEST.MISCMASK
 *
 * These default masks may be replaced by user-defined ones (specified in the manifest file).
 * Corresponding manifest keys are:
 *   - `sgx.seal_key.flags_mask`
 *   - `sgx.seal_key.xfrm_mask`
 *   - `sgx.seal_key.misc_mask`
 */
static uint64_t g_seal_key_flags_mask = SGX_FLAGS_MASK_CONST;
static uint64_t g_seal_key_xfrm_mask  = SGX_XFRM_MASK_CONST;
static uint32_t g_seal_key_misc_mask  = SGX_MISCSELECT_MASK_CONST;

static_assert(sizeof(g_seal_key_flags_mask) + sizeof(g_seal_key_xfrm_mask) ==
                  sizeof(sgx_attributes_t), "wrong types");
static_assert(sizeof(g_seal_key_misc_mask) == sizeof(sgx_misc_select_t), "wrong types");

bool sgx_is_completely_within_enclave(const void* _addr, size_t size) {
    uintptr_t addr = (uintptr_t)_addr;
    if (addr > UINTPTR_MAX - size) {
        return false;
    }

    return g_enclave_base <= addr && addr + size <= g_enclave_top;
}

bool sgx_is_valid_untrusted_ptr(const void* _addr, size_t size, size_t alignment) {
    uintptr_t addr = (uintptr_t)_addr;
    if (addr > UINTPTR_MAX - size) {
        return false;
    }

    if (!(addr + size <= g_enclave_base || g_enclave_top <= addr)) {
        return false;
    }

    return IS_ALIGNED(addr, alignment);
}

/*
 * When DEBUG is enabled, we run sgx_profile_sample() during asynchronous enclave exit (AEX), which
 * uses the stack. Make sure to update URSP so that the AEX handler does not overwrite the part of
 * the stack that we just allocated.
 *
 * (Recall that URSP is an outside stack pointer, saved by EENTER and restored on AEX by the SGX
 * hardware itself.)
 */
#ifdef DEBUG

#define UPDATE_USTACK(_ustack)                           \
    do {                                                 \
        SET_ENCLAVE_TCB(ustack, _ustack);                \
        GET_ENCLAVE_TCB(gpr)->ursp = (uint64_t)_ustack;  \
    } while(0)

#else

#define UPDATE_USTACK(_ustack) SET_ENCLAVE_TCB(ustack, _ustack)

#endif

void* sgx_prepare_ustack(void) {
    void* old_ustack = GET_ENCLAVE_TCB(ustack);

    void* ustack = old_ustack;
    if (ustack != GET_ENCLAVE_TCB(ustack_top))
        ustack -= RED_ZONE_SIZE;
    UPDATE_USTACK(ustack);

    return old_ustack;
}

void* sgx_alloc_on_ustack_aligned(size_t size, size_t alignment) {
    assert(IS_POWER_OF_2(alignment));
    void* ustack = GET_ENCLAVE_TCB(ustack) - size;
    ustack = ALIGN_DOWN_PTR_POW2(ustack, alignment);
    if (!sgx_is_valid_untrusted_ptr(ustack, size, alignment)) {
        return NULL;
    }
    UPDATE_USTACK(ustack);
    return ustack;
}

void* sgx_alloc_on_ustack(size_t size) {
    return sgx_alloc_on_ustack_aligned(size, 1);
}

void* sgx_copy_to_ustack(const void* ptr, size_t size) {
    if (!sgx_is_completely_within_enclave(ptr, size)) {
        return NULL;
    }
    void* uptr = sgx_alloc_on_ustack(size);
    if (uptr) {
        sgx_copy_from_enclave_verified(uptr, ptr, size);
    }
    return uptr;
}

void sgx_reset_ustack(const void* old_ustack) {
    assert(old_ustack <= GET_ENCLAVE_TCB(ustack_top));
    UPDATE_USTACK(old_ustack);
}

static void copy_u64s(void* dst, const void* src, size_t count) {
    __asm__ volatile (
        "rep movsq\n"
        : "+D"(dst), "+S"(src), "+c"(count)
        :
        : "memory", "cc"
    );
}

static void copy_u64s_from_untrusted(void* dst, const void* untrusted_src, size_t count) {
    assert((uintptr_t)untrusted_src % 8 == 0);
    copy_u64s(dst, untrusted_src, count);
}

static void copy_u64s_to_untrusted(void* untrusted_dst, const void* src, size_t count) {
    assert((uintptr_t)untrusted_dst % 8 == 0);

    copy_u64s(untrusted_dst, src, count);
}

void sgx_copy_to_enclave_verified(void* ptr, const void* uptr, size_t size) {
    assert(sgx_is_valid_untrusted_ptr(uptr, size, /*alignment=*/1));
    assert(sgx_is_completely_within_enclave(ptr, size));

    if (size == 0) {
        return;
    }

    /*
     * This should be simple `memcpy(ptr, uptr, size)`, but CVE-2022-21233 (INTEL-SA-00657).
     * To mitigate this issue, all reads of untrusted memory from within the enclave must be done
     * in 8-byte chunks aligned to 8-bytes boundary. Since x64 allocates memory in pages of
     * (at least) 0x1000 in size, we can safely 8-align the pointer down and the size up.
     */
    size_t copy_len;
    size_t prefix_misalignment = (uintptr_t)uptr & 7;
    if (prefix_misalignment) {
        /* Beginning of the copied range is misaligned. */
        char prefix_val[8] = { 0 };
        copy_u64s_from_untrusted(prefix_val, (char*)uptr - prefix_misalignment, /*count=*/1);

        copy_len = MIN(sizeof(prefix_val) - prefix_misalignment, size);
        memcpy(ptr, prefix_val + prefix_misalignment, copy_len);
        ptr = (char*)ptr + copy_len;
        uptr = (const char*)uptr + copy_len;
        size -= copy_len;

        if (size == 0) {
            return;
        }
    }
    assert((uintptr_t)uptr % 8 == 0);

    size_t suffix_misalignment = size & 7;
    copy_len = size - suffix_misalignment;
    assert(copy_len % 8 == 0);
    copy_u64s_from_untrusted(ptr, uptr, copy_len / 8);
    ptr = (char*)ptr + copy_len;
    uptr = (const char*)uptr + copy_len;
    size -= copy_len;

    assert(size == suffix_misalignment);
    if (suffix_misalignment) {
        /* End of the copied range is misaligned. */
        char suffix_val[8] = { 0 };
        copy_u64s_from_untrusted(suffix_val, uptr, /*count=*/1);
        memcpy(ptr, suffix_val, suffix_misalignment);
    }
}

bool sgx_copy_to_enclave(void* ptr, size_t maxsize, const void* uptr, size_t usize) {
    if (usize > maxsize ||
        !sgx_is_valid_untrusted_ptr(uptr, usize, /*alignment=*/1) ||
        !sgx_is_completely_within_enclave(ptr, maxsize)) {
        return false;
    }
    sgx_copy_to_enclave_verified(ptr, uptr, usize);
    return true;
}

void sgx_copy_from_enclave_verified(void* uptr, const void* ptr, size_t size) {
    assert(sgx_is_completely_within_enclave(ptr, size));
    assert(sgx_is_valid_untrusted_ptr(uptr, size, /*alignment=*/1));

    if (size == 0) {
        return;
    }

    /*
     * This should be simple `memcpy(uptr, ptr, size)`, but CVE-2022-21166 (INTEL-SA-00615).
     * To mitigate this issue, all writes to untrusted memory from within the enclave must be done
     * in 8-byte chunks aligned to 8-bytes boundary. Since x64 allocates memory in pages of
     * (at least) 0x1000 in size, we can safely 8-align the pointer down and the size up.
     */
    size_t copy_len;
    size_t prefix_misalignment = (uintptr_t)uptr & 7;
    if (prefix_misalignment) {
        /* Beginning of the range to copy is misaligned. */
        char prefix_val[8] = { 0 };
        copy_len = MIN(sizeof(prefix_val) - prefix_misalignment, size);

        copy_u64s_from_untrusted(prefix_val, (char*)uptr - prefix_misalignment, /*count=*/1);
        memcpy(prefix_val + prefix_misalignment, ptr, copy_len);
        copy_u64s_to_untrusted((char*)uptr - prefix_misalignment, prefix_val, /*count=*/1);
        uptr = (char*)uptr + copy_len;
        ptr = (const char*)ptr + copy_len;
        size -= copy_len;

        if (size == 0) {
            return;
        }
    }
    assert((uintptr_t)uptr % 8 == 0);

    size_t suffix_misalignment = size & 7;
    copy_len = size - suffix_misalignment;
    assert(copy_len % 8 == 0);
    copy_u64s_to_untrusted(uptr, ptr, copy_len / 8);
    uptr = (char*)uptr + copy_len;
    ptr = (const char*)ptr + copy_len;
    size -= copy_len;

    assert(size == suffix_misalignment);
    if (suffix_misalignment) {
        /* End of the range to copy is misaligned. */
        char suffix_val[8] = { 0 };

        copy_u64s_from_untrusted(suffix_val, uptr, /*count=*/1);
        memcpy(suffix_val, ptr, suffix_misalignment);
        copy_u64s_to_untrusted(uptr, suffix_val, 1);
    }
}

bool sgx_copy_from_enclave(void* uptr, const void* ptr, size_t size) {
    if (!sgx_is_valid_untrusted_ptr(uptr, size, /*alignment=*/1)
            || !sgx_is_completely_within_enclave(ptr, size)) {
        return false;
    }

    sgx_copy_from_enclave_verified(uptr, ptr, size);
    return true;
}

void* sgx_import_array_to_enclave(const void* uptr, size_t elem_size, size_t elem_cnt) {
    size_t size;
    if (__builtin_mul_overflow(elem_size, elem_cnt, &size))
        return NULL;

    void* buf = malloc(size);
    if (!buf) {
        return NULL;
    }

    if (!sgx_copy_to_enclave(buf, size, uptr, size)) {
        free(buf);
        return NULL;
    }

    return buf;
}

void* sgx_import_array2d_to_enclave(const void* uptr, size_t elem_size, size_t elem_cnt1,
                                    size_t elem_cnt2) {
    size_t elem_cnt;
    if (__builtin_mul_overflow(elem_cnt1, elem_cnt2, &elem_cnt))
        return NULL;

    return sgx_import_array_to_enclave(uptr, elem_size, elem_cnt);
}

static void print_report(sgx_report_t* r) {
    char hex[64 * 2 + 1]; /* large enough to hold any of the below fields */

#define BYTES2HEX(bytes) (bytes2hex(bytes, sizeof(bytes), hex, sizeof(hex)))
    log_debug("  cpu_svn:     %s",     BYTES2HEX(r->body.cpu_svn.svn));
    log_debug("  misc_select: %08x",   r->body.misc_select);
    log_debug("  mr_enclave:  %s",     BYTES2HEX(r->body.mr_enclave.m));
    log_debug("  mr_signer:   %s",     BYTES2HEX(r->body.mr_signer.m));
    log_debug("  attr.flags:  %016lx", r->body.attributes.flags);
    log_debug("  attr.xfrm:   %016lx", r->body.attributes.xfrm);
    log_debug("  isv_prod_id: %02x",   r->body.isv_prod_id);
    log_debug("  isv_svn:     %02x",   r->body.isv_svn);
    log_debug("  report_data: %s",     BYTES2HEX(r->body.report_data.d));
    log_debug("  key_id:      %s",     BYTES2HEX(r->key_id.id));
    log_debug("  mac:         %s",     BYTES2HEX(r->mac));
#undef BYTES2HEX
}

int sgx_get_report(const sgx_target_info_t* target_info, const sgx_report_data_t* data,
                   sgx_report_t* report) {
    int ret = sgx_report(target_info, data, report);
    if (ret) {
        log_error("sgx_report failed: %s", unix_strerror(ret));
        return -PAL_ERROR_DENIED;
    }
    return 0;
}

int sgx_verify_report(sgx_report_t* report) {
    __sgx_mem_aligned sgx_key_request_t keyrequest;
    memset(&keyrequest, 0, sizeof(sgx_key_request_t));
    keyrequest.key_name = SGX_REPORT_KEY;
    memcpy(&keyrequest.key_id, &report->key_id, sizeof(keyrequest.key_id));

    sgx_key_128bit_t report_key __attribute__((aligned(sizeof(sgx_key_128bit_t))));
    memset(&report_key, 0, sizeof(report_key));

    int ret = sgx_getkey(&keyrequest, &report_key);
    if (ret) {
        log_error("Can't get report key");
        return -PAL_ERROR_DENIED;
    }

    sgx_mac_t check_mac;
    memset(&check_mac, 0, sizeof(check_mac));

    // Generating the MAC with AES-CMAC using the report key. Only hash the part of the report
    // BEFORE the keyid field (hence the offsetof(...) trick). ENCLU[EREPORT] does not include
    // the MAC and the keyid fields when generating the MAC.
    lib_AESCMAC((uint8_t*)&report_key, sizeof(report_key),
                (uint8_t*)report, offsetof(sgx_report_t, key_id),
                (uint8_t*)&check_mac, sizeof(check_mac));

    // Clear the report key for security
    erase_memory(&report_key, sizeof(report_key));

    log_debug("Verify report:");
    print_report(report);

    if (!ct_memequal(&check_mac, &report->mac, sizeof(check_mac))) {
        log_error("Report verification failed");
        return -PAL_ERROR_DENIED;
    }

    return 0;
}

int sgx_get_seal_key(uint16_t key_policy, sgx_key_128bit_t* out_seal_key) {
    assert(key_policy == SGX_KEYPOLICY_MRENCLAVE || key_policy == SGX_KEYPOLICY_MRSIGNER);

    /* The keyrequest struct dictates the key derivation material used to generate the sealing key.
     * It includes MRENCLAVE/MRSIGNER key policy (to allow secret migration/sealing between
     * instances of the same enclave or between different enclaves of the same author/signer),
     * CPU/ISV/CONFIG SVNs (to prevent secret migration to older vulnerable versions of the
     * enclave), ATTRIBUTES and MISCSELECT masks (to prevent secret migration from e.g. production
     * enclave to debug enclave). Note that KEYID is zero, to generate the same sealing key in
     * different instances of the same enclave/same signer. */
    __sgx_mem_aligned sgx_key_request_t key_request = {0};
    key_request.key_name   = SGX_SEAL_KEY;
    key_request.key_policy = key_policy;

    memcpy(&key_request.cpu_svn, &g_pal_linuxsgx_state.enclave_info.cpu_svn, sizeof(sgx_cpu_svn_t));
    memcpy(&key_request.isv_svn, &g_pal_linuxsgx_state.enclave_info.isv_svn, sizeof(sgx_isv_svn_t));
    memcpy(&key_request.config_svn, &g_pal_linuxsgx_state.enclave_info.config_svn,
           sizeof(sgx_config_svn_t));

    key_request.attribute_mask.flags = g_seal_key_flags_mask;
    key_request.attribute_mask.xfrm  = g_seal_key_xfrm_mask;
    key_request.misc_mask            = g_seal_key_misc_mask;

    int ret = sgx_getkey(&key_request, out_seal_key);
    if (ret) {
        log_error("Failed to generate sealing key using SGX EGETKEY");
        return -PAL_ERROR_DENIED;
    }
    return 0;
}

DEFINE_LISTP(trusted_file);
static LISTP_TYPE(trusted_file) g_trusted_file_list = LISTP_INIT;
static spinlock_t g_trusted_file_lock = INIT_SPINLOCK_UNLOCKED;
static int g_file_check_policy = FILE_CHECK_POLICY_STRICT;

static void find_path_in_uri(const char* uri, size_t uri_len, const char** out_path,
                             size_t* out_path_len) {
    if (strstartswith(uri, URI_PREFIX_FILE)) {
        *out_path = uri + URI_PREFIX_FILE_LEN;
        *out_path_len = uri_len - URI_PREFIX_FILE_LEN;
        return;
    }

    assert(strstartswith(uri, URI_PREFIX_DEV));
    *out_path = uri + URI_PREFIX_DEV_LEN;
    *out_path_len = uri_len - URI_PREFIX_DEV_LEN;
}

/* assumes `path` is normalized */
static bool path_is_equal_or_subpath(const struct trusted_file* tf, const char* path,
                                     size_t path_len) {
    const char* tf_path;
    size_t tf_path_len;
    find_path_in_uri(tf->uri, tf->uri_len, &tf_path, &tf_path_len);

    if (tf_path_len > path_len || memcmp(tf_path, path, tf_path_len)) {
        /* tf path is not a prefix of `path` */
        return false;
    }
    if (tf_path_len == path_len) {
        /* Both are equal */
        return true;
    }
    if (tf_path[tf_path_len - 1] == '/') {
        /* tf path is a subpath of `path` (with slash), e.g. "foo/" and "foo/bar" */
        return true;
    }
    if (path[tf_path_len] == '/') {
        /* tf path is a subpath of `path` (without slash), e.g. "foo" and "foo/bar" */
        return true;
    }
    return false;
}

struct trusted_file* get_trusted_or_allowed_file(const char* path) {
    struct trusted_file* tf = NULL;

    size_t path_len = strlen(path);

    spinlock_lock(&g_trusted_file_lock);

    struct trusted_file* tmp;
    LISTP_FOR_EACH_ENTRY(tmp, &g_trusted_file_list, list) {
        if (tmp->allowed) {
            /* allowed files: must be a subfolder or file */
            if (path_is_equal_or_subpath(tmp, path, path_len)) {
                tf = tmp;
                break;
            }
        } else {
            /* trusted files: must be exactly the same URI */
            const char* tf_path;
            size_t tf_path_len;
            find_path_in_uri(tmp->uri, tmp->uri_len, &tf_path, &tf_path_len);
            if (tf_path_len == path_len && !memcmp(tf_path, path, path_len + 1)) {
                tf = tmp;
                break;
            }
        }
    }

    spinlock_unlock(&g_trusted_file_lock);

    return tf;
}

int load_trusted_or_allowed_file(struct trusted_file* tf, PAL_HANDLE file, bool create,
                                 sgx_chunk_hash_t** out_chunk_hashes, uint64_t* out_size,
                                 void** out_umem) {
    int ret;

    *out_chunk_hashes = NULL;
    *out_size = 0;
    *out_umem = NULL;

    if (create) {
        assert(tf->allowed);
        return register_file(tf->uri, /*hash_str=*/NULL, /*check_duplicates=*/true);
    }

    if (tf->allowed) {
        /* allowed files: do not need any integrity, so no need for chunk hashes */
        return 0;
    }

    /* trusted files: need integrity, so calculate chunk hashes and compare with hash in manifest */
    if (!file->file.seekable) {
        log_warning("Trusted file '%s' is not seekable, cannot load it", file->file.realpath);
        return -PAL_ERROR_DENIED;
    }

    sgx_chunk_hash_t* chunk_hashes = NULL;
    uint8_t* tmp_chunk = NULL; /* scratch buf to calculate whole-file and chunk-of-file hashes */

    /* mmap the whole trusted file in untrusted memory for future reads/writes; it is
     * caller's responsibility to unmap those areas after use */
    *out_size = tf->size;
    if (*out_size) {
        ret = ocall_mmap_untrusted(out_umem, tf->size, PROT_READ, MAP_SHARED, file->file.fd,
                                   /*offset=*/0);
        if (ret < 0) {
            *out_umem = NULL;
            ret = unix_to_pal_error(ret);
            goto fail;
        }
    }

    spinlock_lock(&g_trusted_file_lock);
    if (tf->chunk_hashes) {
        *out_chunk_hashes = tf->chunk_hashes;
        spinlock_unlock(&g_trusted_file_lock);
        return 0;
    }
    spinlock_unlock(&g_trusted_file_lock);

    chunk_hashes = malloc(sizeof(sgx_chunk_hash_t) * UDIV_ROUND_UP(tf->size, TRUSTED_CHUNK_SIZE));
    if (!chunk_hashes) {
        ret = -PAL_ERROR_NOMEM;
        goto fail;
    }

    tmp_chunk = malloc(TRUSTED_CHUNK_SIZE);
    if (!tmp_chunk) {
        ret = -PAL_ERROR_NOMEM;
        goto fail;
    }

    sgx_chunk_hash_t* chunk_hashes_item = chunk_hashes;
    uint64_t offset = 0;
    LIB_SHA256_CONTEXT file_sha;

    ret = lib_SHA256Init(&file_sha);
    if (ret < 0)
        goto fail;

    for (; offset < tf->size; offset += TRUSTED_CHUNK_SIZE, chunk_hashes_item++) {
        /* For each file chunk of size TRUSTED_CHUNK_SIZE, generate 128-bit hash from SHA-256 hash
         * over contents of this file chunk (we simply truncate SHA-256 hash to first 128 bits; this
         * is fine for integrity purposes). Also, generate a SHA-256 hash for the whole file
         * contents to compare with the manifest "reference" hash value. */
        uint64_t chunk_size = MIN(tf->size - offset, TRUSTED_CHUNK_SIZE);
        LIB_SHA256_CONTEXT chunk_sha;
        ret = lib_SHA256Init(&chunk_sha);
        if (ret < 0)
            goto fail;

        /* to prevent TOCTOU attacks, copy file contents into the enclave before hashing */
        if (!sgx_copy_to_enclave(tmp_chunk, TRUSTED_CHUNK_SIZE, *out_umem + offset, chunk_size))
            goto fail;

        ret = lib_SHA256Update(&file_sha, tmp_chunk, chunk_size);
        if (ret < 0)
            goto fail;

        ret = lib_SHA256Update(&chunk_sha, tmp_chunk, chunk_size);
        if (ret < 0)
            goto fail;

        sgx_chunk_hash_t chunk_hash[2]; /* each chunk_hash is 128 bits in size */
        static_assert(sizeof(chunk_hash) * 8 == 256, "");
        ret = lib_SHA256Final(&chunk_sha, (uint8_t*)&chunk_hash[0]);
        if (ret < 0)
            goto fail;

        /* note that we truncate SHA256 to 128 bits */
        memcpy(chunk_hashes_item, &chunk_hash[0], sizeof(*chunk_hashes_item));
    }

    sgx_file_hash_t file_hash;
    ret = lib_SHA256Final(&file_sha, file_hash.bytes);
    if (ret < 0)
        goto fail;

    /* check the generated hash-over-whole-file against the reference hash in the manifest */
    if (memcmp(&file_hash, &tf->file_hash, sizeof(file_hash))) {
        log_warning("Hash of trusted file '%s' does not match with the reference hash in manifest",
                    file->file.realpath);
        ret = -PAL_ERROR_DENIED;
        goto fail;
    }

    spinlock_lock(&g_trusted_file_lock);
    if (tf->chunk_hashes) {
        *out_chunk_hashes = tf->chunk_hashes;
        spinlock_unlock(&g_trusted_file_lock);
        free(chunk_hashes);
        free(tmp_chunk);
        return 0;
    }
    tf->chunk_hashes = chunk_hashes;
    *out_chunk_hashes = chunk_hashes;
    spinlock_unlock(&g_trusted_file_lock);

    free(tmp_chunk);
    return 0;

fail:
    if (*out_umem) {
        assert(*out_size > 0);
        ocall_munmap_untrusted(*out_umem, *out_size);
    }
    free(chunk_hashes);
    free(tmp_chunk);
    return ret;
}

int get_file_check_policy(void) {
    return g_file_check_policy;
}

static void set_file_check_policy(int policy) {
    g_file_check_policy = policy;
}

int copy_and_verify_trusted_file(const char* path, uint8_t* buf, const void* umem,
                                 off_t aligned_offset, off_t aligned_end, off_t offset, off_t end,
                                 sgx_chunk_hash_t* chunk_hashes, size_t file_size) {
    int ret = 0;

    assert(IS_ALIGNED(aligned_offset, TRUSTED_CHUNK_SIZE));
    assert(offset >= aligned_offset && end <= aligned_end);

    uint8_t* tmp_chunk = malloc(TRUSTED_CHUNK_SIZE);
    if (!tmp_chunk) {
        ret = -PAL_ERROR_NOMEM;
        goto failed;
    }

    sgx_chunk_hash_t* chunk_hashes_item = chunk_hashes + aligned_offset / TRUSTED_CHUNK_SIZE;

    uint8_t* buf_pos = buf;
    off_t chunk_offset = aligned_offset;
    for (; chunk_offset < aligned_end; chunk_offset += TRUSTED_CHUNK_SIZE, chunk_hashes_item++) {
        size_t chunk_size = MIN(file_size - chunk_offset, TRUSTED_CHUNK_SIZE);
        off_t chunk_end   = chunk_offset + chunk_size;

        sgx_chunk_hash_t chunk_hash[2]; /* each chunk_hash is 128 bits in size but we need 256 */

        LIB_SHA256_CONTEXT chunk_sha;
        ret = lib_SHA256Init(&chunk_sha);
        if (ret < 0)
            goto failed;

        if (chunk_offset >= offset && chunk_end <= end) {
            /* if current chunk-to-copy completely resides in the requested region-to-copy,
             * directly copy into buf (without a scratch buffer) and hash in-place */
            if (!sgx_copy_to_enclave(buf_pos, chunk_size, umem + chunk_offset, chunk_size)) {
                goto failed;
            }

            ret = lib_SHA256Update(&chunk_sha, buf_pos, chunk_size);
            if (ret < 0)
                goto failed;

            buf_pos += chunk_size;
        } else {
            /* if current chunk-to-copy only partially overlaps with the requested region-to-copy,
             * read the file contents into a scratch buffer, verify hash and then copy only the part
             * needed by the caller */
            if (!sgx_copy_to_enclave(tmp_chunk, chunk_size, umem + chunk_offset, chunk_size)) {
                goto failed;
            }

            ret = lib_SHA256Update(&chunk_sha, tmp_chunk, chunk_size);
            if (ret < 0)
                goto failed;

            /* determine which part of the chunk is needed by the caller */
            off_t copy_start = MAX(chunk_offset, offset);
            off_t copy_end   = MIN(chunk_offset + (off_t)chunk_size, end);
            assert(copy_end > copy_start);

            memcpy(buf_pos, tmp_chunk + copy_start - chunk_offset, copy_end - copy_start);
            buf_pos += copy_end - copy_start;
        }

        ret = lib_SHA256Final(&chunk_sha, (uint8_t*)&chunk_hash[0]);
        if (ret < 0)
            goto failed;

        if (memcmp(chunk_hashes_item, &chunk_hash[0], sizeof(*chunk_hashes_item))) {
            log_error("Accessing file '%s' is denied: incorrect hash of file chunk at %lu-%lu.",
                      path, chunk_offset, chunk_end);
            ret = -PAL_ERROR_DENIED;
            goto failed;
        }
    }

    free(tmp_chunk);
    return 0;

failed:
    free(tmp_chunk);
    memset(buf, 0, end - offset);
    return ret;
}

static int register_file(const char* uri, const char* hash_str, bool check_duplicates) {
    if (hash_str && strlen(hash_str) != sizeof(sgx_file_hash_t) * 2) {
        log_error("Hash (%s) of a trusted file %s is not a SHA256 hash", hash_str, uri);
        return -PAL_ERROR_INVAL;
    }

    size_t uri_len = strlen(uri);
    if (uri_len >= URI_MAX) {
        log_error("Size of file exceeds maximum %dB: %s", URI_MAX, uri);
        return -PAL_ERROR_INVAL;
    }

    if (check_duplicates) {
        /* this check is only done during runtime (when creating a new file) and not needed during
         * initialization (because manifest is assumed to have no duplicates); skipping this check
         * significantly improves startup time */
        spinlock_lock(&g_trusted_file_lock);
        struct trusted_file* tf;
        LISTP_FOR_EACH_ENTRY(tf, &g_trusted_file_list, list) {
            if (tf->uri_len == uri_len && !memcmp(tf->uri, uri, uri_len)) {
                spinlock_unlock(&g_trusted_file_lock);
                return 0;
            }
        }
        spinlock_unlock(&g_trusted_file_lock);
    }

    struct trusted_file* new = malloc(sizeof(*new) + uri_len + 1);
    if (!new)
        return -PAL_ERROR_NOMEM;

    INIT_LIST_HEAD(new, list);
    new->size = 0;
    new->chunk_hashes = NULL;
    new->allowed = false;
    new->uri_len = uri_len;
    memcpy(new->uri, uri, uri_len + 1);

    if (hash_str) {
        assert(strlen(hash_str) == sizeof(sgx_file_hash_t) * 2);

        char* bytes = hex2bytes(hash_str, strlen(hash_str), new->file_hash.bytes,
                                sizeof(new->file_hash.bytes));
        if (!bytes) {
            log_error("Could not parse hash of file: %s", uri);
            free(new);
            return -PAL_ERROR_INVAL;
        }
    } else {
        memset(&new->file_hash, 0, sizeof(new->file_hash));
        new->allowed = true;
    }

    spinlock_lock(&g_trusted_file_lock);

    if (check_duplicates) {
        /* this check is only done during runtime and not needed during initialization (see above);
         * we check again because same file could have been added by another thread in meantime */
        struct trusted_file* tf;
        LISTP_FOR_EACH_ENTRY(tf, &g_trusted_file_list, list) {
            if (tf->uri_len == uri_len && !memcmp(tf->uri, uri, uri_len)) {
                spinlock_unlock(&g_trusted_file_lock);
                free(new);
                return 0;
            }
        }
    }

    LISTP_ADD_TAIL(new, &g_trusted_file_list, list);
    spinlock_unlock(&g_trusted_file_lock);

    return 0;
}

static int normalize_and_register_file(const char* uri, const char* hash_str) {
    int ret;

    if (hash_str) {
        if (!strstartswith(uri, URI_PREFIX_FILE)) {
            log_error("Invalid URI [%s]: Trusted files must start with 'file:'", uri);
            return -PAL_ERROR_INVAL;
        }
    } else {
        if (!strstartswith(uri, URI_PREFIX_FILE) && !strstartswith(uri, URI_PREFIX_DEV)) {
            log_error("Invalid URI [%s]: Allowed files must start with 'file:' or 'dev:'", uri);
            return -PAL_ERROR_INVAL;
        }
    }

    const size_t norm_uri_size = strlen(uri) + 1;
    char* norm_uri = malloc(norm_uri_size);
    if (!norm_uri) {
        return -PAL_ERROR_NOMEM;
    }

    const char* uri_prefix;
    size_t uri_prefix_len;
    if (strstartswith(uri, URI_PREFIX_FILE)) {
        uri_prefix = URI_PREFIX_FILE;
        uri_prefix_len = URI_PREFIX_FILE_LEN;
    } else {
        assert(strstartswith(uri, URI_PREFIX_DEV));
        uri_prefix = URI_PREFIX_DEV;
        uri_prefix_len = URI_PREFIX_DEV_LEN;
    }

    memcpy(norm_uri, uri_prefix, uri_prefix_len);
    size_t norm_path_size = norm_uri_size - uri_prefix_len;
    if (!get_norm_path(uri + uri_prefix_len, norm_uri + uri_prefix_len, &norm_path_size)) {
        log_error("Path (%s) normalization failed", uri);
        ret = -PAL_ERROR_INVAL;
        goto out;
    }

    ret = register_file(norm_uri, hash_str, /*check_duplicates=*/false);
out:
    free(norm_uri);
    return ret;
}

int init_trusted_files(void) {
    int ret;

    toml_table_t* manifest_sgx = toml_table_in(g_pal_public_state.manifest_root, "sgx");
    if (!manifest_sgx)
        return 0;

    toml_array_t* toml_trusted_files = toml_array_in(manifest_sgx, "trusted_files");
    if (!toml_trusted_files)
        return 0;

    ssize_t toml_trusted_files_cnt = toml_array_nelem(toml_trusted_files);
    if (toml_trusted_files_cnt < 0)
        return -PAL_ERROR_DENIED;
    if (toml_trusted_files_cnt == 0)
        return 0;

    char* toml_trusted_uri_str = NULL;
    char* toml_trusted_sha256_str = NULL;

    for (ssize_t i = 0; i < toml_trusted_files_cnt; i++) {
        /* read `sgx.trusted_file = {uri = "file:foo", sha256 = "deadbeef"}` entry from manifest */
        toml_table_t* toml_trusted_file = toml_table_at(toml_trusted_files, i);
        if (!toml_trusted_file) {
            log_error("Invalid trusted file in manifest at index %ld (not a TOML table)", i);
            ret = -PAL_ERROR_INVAL;
            goto out;
        }

        toml_raw_t toml_trusted_uri_raw = toml_raw_in(toml_trusted_file, "uri");
        if (!toml_trusted_uri_raw) {
            log_error("Invalid trusted file in manifest at index %ld (no 'uri' key)", i);
            ret = -PAL_ERROR_INVAL;
            goto out;
        }

        ret = toml_rtos(toml_trusted_uri_raw, &toml_trusted_uri_str);
        if (ret < 0) {
            log_error("Invalid trusted file in manifest at index %ld ('uri' is not a string)", i);
            ret = -PAL_ERROR_INVAL;
            goto out;
        }

        /*toml_raw_t toml_trusted_sha256_raw = toml_raw_in(toml_trusted_file, "sha256");
        if (!toml_trusted_sha256_raw) {
            log_error("Invalid trusted file in manifest at index %ld (no 'sha256' key)", i);
            ret = -PAL_ERROR_INVAL;
            goto out;
        }

        ret = toml_rtos(toml_trusted_sha256_raw, &toml_trusted_sha256_str);
        if (ret < 0 || !toml_trusted_sha256_str) {
            log_error("Invalid trusted file in manifest at index %ld ('sha256' is not a string)",
                      i);
            ret = -PAL_ERROR_INVAL;
            goto out;
        }*/

        ret = normalize_and_register_file(toml_trusted_uri_str, toml_trusted_sha256_str);
        if (ret < 0) {
            log_error("normalize_and_register_file(\"%s\", \"%s\") failed with error code: %s",
                      toml_trusted_uri_str, toml_trusted_sha256_str, pal_strerror(ret));
            goto out;
        }

        free(toml_trusted_uri_str);
        free(toml_trusted_sha256_str);
        toml_trusted_uri_str = NULL;
        toml_trusted_sha256_str = NULL;
    }

    ret = 0;
out:
    free(toml_trusted_uri_str);
    free(toml_trusted_sha256_str);
    return ret;
}

static void maybe_warn_about_allowed_files_usage(void) {
    if (!g_pal_common_state.parent_process)
        g_allowed_files_warn = true;
}

int init_allowed_files(void) {
    int ret;

    toml_table_t* manifest_sgx = toml_table_in(g_pal_public_state.manifest_root, "sgx");
    if (!manifest_sgx)
        return 0;

    toml_array_t* toml_allowed_files = toml_array_in(manifest_sgx, "allowed_files");
    if (!toml_allowed_files)
        return 0;

    maybe_warn_about_allowed_files_usage();

    ssize_t toml_allowed_files_cnt = toml_array_nelem(toml_allowed_files);
    if (toml_allowed_files_cnt < 0)
        return -PAL_ERROR_DENIED;
    if (toml_allowed_files_cnt == 0)
        return 0;

    char* toml_allowed_file_str = NULL;

    for (ssize_t i = 0; i < toml_allowed_files_cnt; i++) {
        toml_raw_t toml_allowed_file_raw = toml_raw_at(toml_allowed_files, i);
        if (!toml_allowed_file_raw) {
            log_error("Invalid allowed file in manifest at index %ld", i);
            ret = -PAL_ERROR_INVAL;
            goto out;
        }

        ret = toml_rtos(toml_allowed_file_raw, &toml_allowed_file_str);
        if (ret < 0) {
            log_error("Invalid allowed file in manifest at index %ld (not a string)", i);
            ret = -PAL_ERROR_INVAL;
            goto out;
        }

        ret = normalize_and_register_file(toml_allowed_file_str, /*hash_str=*/NULL);
        if (ret < 0) {
            log_error("normalize_and_register_file(\"%s\", NULL) failed with error: %s",
                      toml_allowed_file_str, pal_strerror(ret));
            goto out;
        }

        free(toml_allowed_file_str);
        toml_allowed_file_str = NULL;
    }

    ret = 0;
out:
    free(toml_allowed_file_str);
    return ret;
}

int init_file_check_policy(void) {
    int ret;

    char* file_check_policy_str = NULL;
    ret = toml_string_in(g_pal_public_state.manifest_root, "sgx.file_check_policy",
                         &file_check_policy_str);
    if (ret < 0) {
        log_error("Cannot parse 'sgx.file_check_policy'");
        return -PAL_ERROR_INVAL;
    }

    if (!file_check_policy_str)
        return 0;

    if (!strcmp(file_check_policy_str, "strict")) {
        set_file_check_policy(FILE_CHECK_POLICY_STRICT);
    } else if (!strcmp(file_check_policy_str, "allow_all_but_log")) {
        set_file_check_policy(FILE_CHECK_POLICY_ALLOW_ALL_BUT_LOG);
    } else {
        log_error("Unknown value for 'sgx.file_check_policy' "
                  "(allowed: `strict`, `allow_all_but_log`)'");
        free(file_check_policy_str);
        return -PAL_ERROR_INVAL;
    }

    log_debug("File check policy: %s", file_check_policy_str);
    free(file_check_policy_str);
    return 0;
}

static int update_seal_key_mask(const char* mask_name, uint8_t* mask_ptr, size_t mask_size) {
    int ret;

    char* mask_str = NULL;
    ret = toml_string_in(g_pal_public_state.manifest_root, mask_name, &mask_str);
    if (ret < 0) {
        log_error("Cannot parse '%s'", mask_name);
        return -PAL_ERROR_INVAL;
    }

    if (!mask_str) {
        /* no mask specified in the manifest, use the default */
        return 0;
    }

    if (strlen(mask_str) != 2 + mask_size * 2) {
        log_error("Malformed '%s' value in the manifest (wrong size)", mask_name);
        ret = -PAL_ERROR_INVAL;
        goto out;
    }

    if (mask_str[0] != '0' || (mask_str[1] != 'x' && mask_str[1] != 'X')) {
        log_error("Malformed '%s' value in the manifest (must start with '0x')", mask_name);
        ret = -PAL_ERROR_INVAL;
        goto out;
    }

    memset(mask_ptr, 0, mask_size);

    for (size_t i = 0; i < mask_size * 2; i++) {
        int8_t val = hex2dec(mask_str[i + 2]); /* skip first two chars (the '0x' prefix) */
        if (val < 0) {
            log_error("Malformed '%s' value in the manifest (not a hex number)", mask_name);
            ret = -PAL_ERROR_INVAL;
            goto out;
        }
        uint8_t* mask_byte = &mask_ptr[mask_size - i / 2 - 1];
        *mask_byte = *mask_byte * 16 + (uint8_t)val;
    }

    ret = 0;
out:
    free(mask_str);
    return ret;
}

int init_seal_key_material(void) {
    int ret;

    /* below parsing of TOML strings assumes little-endianness (which is true for this SGX PAL) */
    ret = update_seal_key_mask("sgx.seal_key.flags_mask", (uint8_t*)&g_seal_key_flags_mask,
                               sizeof(g_seal_key_flags_mask));
    if (ret < 0)
        return ret;

    ret = update_seal_key_mask("sgx.seal_key.xfrm_mask", (uint8_t*)&g_seal_key_xfrm_mask,
                               sizeof(g_seal_key_xfrm_mask));
    if (ret < 0)
        return ret;

    return update_seal_key_mask("sgx.seal_key.misc_mask", (uint8_t*)&g_seal_key_misc_mask,
                                sizeof(g_seal_key_misc_mask));
}

int init_enclave(void) {
    //Nothing to do here.
    /* initialize enclave information (MRENCLAVE, etc.) of current enclave (via EREPORT) */
    //__sgx_mem_aligned sgx_target_info_t targetinfo = {0};
    //__sgx_mem_aligned sgx_report_data_t reportdata = {0};
    //__sgx_mem_aligned sgx_report_t report;

    //int ret = sgx_report(&targetinfo, &reportdata, &report);
    //if (ret) {
    //    log_error("Failed to get SGX report for current enclave");
    //    return -PAL_ERROR_INVAL;
    //}

    //memcpy(&g_pal_linuxsgx_state.enclave_info, &report.body,
    //       sizeof(g_pal_linuxsgx_state.enclave_info));
    return 0;
}

static int hash_over_session_key(const PAL_SESSION_KEY* session_key, const char* tag,
                                 size_t tag_size, sgx_report_data_t* out_hash) {
    int ret;
    LIB_SHA256_CONTEXT sha;

    /* SGX report data is 64B in size, but SHA256 hash is only 32B in size, so pad with zeros */
    memset(out_hash, 0, sizeof(*out_hash));

    ret = lib_SHA256Init(&sha);
    if (ret < 0)
        return ret;

    ret = lib_SHA256Update(&sha, (uint8_t*)session_key, sizeof(*session_key));
    if (ret < 0)
        return ret;

    ret = lib_SHA256Update(&sha, (uint8_t*)tag, tag_size);
    if (ret < 0)
        return ret;

    return lib_SHA256Final(&sha, (uint8_t*)out_hash);
}

int _PalStreamKeyExchange(PAL_HANDLE stream, PAL_SESSION_KEY* out_key,
                          sgx_report_data_t* out_parent_report_data,
                          sgx_report_data_t* out_child_report_data) {
    int ret;
    LIB_DH_CONTEXT context;

    uint8_t my_public[DH_SIZE];
    uint8_t peer_public[DH_SIZE];

    size_t secret_size;
    uint8_t secret[DH_SIZE];

    assert(stream->hdr.type == PAL_TYPE_PROCESS);

    /* perform unauthenticated DH key exchange to produce two collaterals: the session key K_e and
     * the assymetric SHA256 hashes over K_e (for later use in SGX report's reportdata) */
    ret = lib_DhInit(&context);
    if (ret < 0)
        return ret;

    ret = lib_DhCreatePublic(&context, my_public, sizeof(my_public));
    if (ret < 0)
        goto out;

    for (int64_t bytes = 0, total = 0; total < (int64_t)sizeof(my_public); total += bytes) {
        bytes = _PalStreamWrite(stream, 0, sizeof(my_public) - total, my_public + total);
        if (bytes < 0) {
            if (bytes == -PAL_ERROR_INTERRUPTED || bytes == -PAL_ERROR_TRYAGAIN) {
                bytes = 0;
                continue;
            }
            ret = (int)bytes;
            goto out;
        }
    }

    for (int64_t bytes = 0, total = 0; total < (int64_t)sizeof(peer_public); total += bytes) {
        bytes = _PalStreamRead(stream, 0, sizeof(peer_public) - total, peer_public + total);
        if (bytes < 0) {
            if (bytes == -PAL_ERROR_INTERRUPTED || bytes == -PAL_ERROR_TRYAGAIN) {
                bytes = 0;
                continue;
            }
            ret = (int)bytes;
            goto out;
        }
        if (bytes == 0) {
            /* peer enclave closed the connection prematurely */
            ret = -PAL_ERROR_DENIED;
            goto out;
        }
    }

    secret_size = sizeof(secret);
    ret = lib_DhCalcSecret(&context, peer_public, sizeof(peer_public), secret, &secret_size);
    if (ret < 0)
        goto out;

    assert(secret_size > 0 && secret_size <= sizeof(secret));

    /* derive the session key K_e using HKDF-SHA256 */
    ret = lib_HKDF_SHA256(secret, secret_size, /*salt=*/NULL, /*salt_size=*/0, /*info=*/NULL,
                          /*info_size=*/0, (uint8_t*)out_key, sizeof(*out_key));
    if (ret < 0)
        goto out;

    /* calculate SHA256(K_e || tag1) / SHA256(K_e || tag2) for use in SGX report's reportdata
     * -- as a proof of key identity during subsequent SGX local attestation; note that parent
     *  enclave A uses the hash (K_e || tag1) and child enclave B uses the hash (K_e || tag2) --
     *  this is to prevent reflection/interleaving attacks */
    ret = hash_over_session_key(out_key, LOCAL_ATTESTATION_TAG_PARENT_STR,
                                sizeof(LOCAL_ATTESTATION_TAG_PARENT_STR), out_parent_report_data);
    if (ret < 0)
        goto out;

    ret = hash_over_session_key(out_key, LOCAL_ATTESTATION_TAG_CHILD_STR,
                                sizeof(LOCAL_ATTESTATION_TAG_CHILD_STR), out_child_report_data);
    if (ret < 0)
        goto out;

    log_debug("Key exchange succeeded");
    ret = 0;
out:
    /* scrub all temporary buffers */
    erase_memory(&secret, sizeof(secret));
    erase_memory(&my_public, sizeof(my_public));
    erase_memory(&peer_public, sizeof(peer_public));
    lib_DhFinal(&context);

    return ret;
}

void sgx_report_body_to_target_info(const sgx_report_body_t* report_body,
                                    sgx_target_info_t* out_target_info) {
    *out_target_info = (sgx_target_info_t){
        .mr_enclave = report_body->mr_enclave,
        .attributes = report_body->attributes,
        .cet_attributes = report_body->cet_attributes,
        .config_svn = report_body->config_svn,
        .misc_select = report_body->misc_select,
        .config_id = report_body->config_id,
    };
}

static void get_current_enclave_target_info(sgx_target_info_t* target_info) {
    sgx_report_body_to_target_info(&g_pal_linuxsgx_state.enclave_info, target_info);
}

static int send_report(PAL_HANDLE stream, sgx_report_t* report) {
    size_t sent = 0;
    while (sent < sizeof(*report)) {
        int64_t ret = _PalStreamWrite(stream, 0, sizeof(*report) - sent, (char*)report + sent);
        if (ret < 0) {
            if (ret == -PAL_ERROR_INTERRUPTED || ret == -PAL_ERROR_TRYAGAIN) {
                continue;
            }
            log_error("Failed to send a report: %s", pal_strerror(ret));
            return ret;
        } else if (ret == 0) {
            log_error("Failed to send a report: unexpected EOF");
            return -PAL_ERROR_INVAL;
        }
        sent += ret;
    }
    return 0;
}

static int recv_report(PAL_HANDLE stream, sgx_report_t* report) {
    size_t got = 0;
    while (got < sizeof(*report)) {
        int64_t ret = _PalStreamRead(stream, 0, sizeof(*report) - got, (char*)report + got);
        if (ret < 0) {
            if (ret == -PAL_ERROR_INTERRUPTED || ret == -PAL_ERROR_TRYAGAIN) {
                continue;
            }
            log_error("Failed to send a report: %s", pal_strerror(ret));
            return ret;
        } else if (ret == 0) {
            log_error("Failed to receive a report: unexpected EOF");
            return -PAL_ERROR_INVAL;
        }
        got += ret;
    }
    return 0;
}

/*
 * Initalize the request of local report exchange.
 *
 * We refer to this enclave as A and to the other enclave as B, e.g., A is this parent enclave and B
 * is the child enclave in the fork case (for more info, see comments in pal_process.c).
 */
int _PalStreamReportRequest(PAL_HANDLE stream, sgx_report_data_t* my_report_data,
                            sgx_report_data_t* peer_report_data) {
    assert(stream->hdr.type == PAL_TYPE_PROCESS);
    int ret;
    __sgx_mem_aligned sgx_report_t report;

    /* B -> A: report[B -> A] */
    ret = recv_report(stream, &report);
    if (ret < 0) {
        return ret;
    }

    log_debug("Received local report");

    /* Verify report[B -> A] */
    ret = sgx_verify_report(&report);
    if (ret < 0) {
        log_error("Failed to verify local report: %s", pal_strerror(ret));
        return ret;
    }

    if (!is_peer_enclave_ok(&report.body, peer_report_data)) {
        log_error("Not an allowed enclave");
        return -PAL_ERROR_DENIED;
    }

    log_debug("Local attestation succeeded!");

    /* Both A and B have the same target info. */
    __sgx_mem_aligned sgx_target_info_t target_info;
    get_current_enclave_target_info(&target_info);

    /* A -> B: report[A -> B] */
    ret = sgx_get_report(&target_info, my_report_data, &report);
    if (ret < 0) {
        log_error("Failed to get local report from CPU: %s", pal_strerror(ret));
        return ret;
    }

    return send_report(stream, &report);
}

/*
 * Respond to the request of local report exchange.
 *
 * We refer to this enclave as B and to the other enclave as A, e.g., B is this child enclave and A
 * is the parent enclave in the fork case (for more info, see comments in pal_process.c).
 */
int _PalStreamReportRespond(PAL_HANDLE stream, sgx_report_data_t* my_report_data,
                            sgx_report_data_t* peer_report_data) {
    assert(stream->hdr.type == PAL_TYPE_PROCESS);
    int ret;
    __sgx_mem_aligned sgx_report_t report;
    __sgx_mem_aligned sgx_target_info_t target_info;

    /* Both A and B have the same target info. */
    get_current_enclave_target_info(&target_info);

    /* B -> A: report[B -> A] */
    ret = sgx_get_report(&target_info, my_report_data, &report);
    if (ret < 0) {
        log_error("Failed to get local report from CPU: %s", pal_strerror(ret));
        return ret;
    }

    ret = send_report(stream, &report);
    if (ret < 0) {
        return ret;
    }

    /* A -> B: report[A -> B] */
    ret = recv_report(stream, &report);
    if (ret < 0) {
        return ret;
    }

    log_debug("Received local report");

    /* Verify report[A -> B] */
    ret = sgx_verify_report(&report);
    if (ret < 0) {
        log_error("Failed to verify local report: %s", pal_strerror(ret));
        return ret;
    }

    if (!is_peer_enclave_ok(&report.body, peer_report_data)) {
        log_error("Not an allowed enclave");
        return -PAL_ERROR_DENIED;
    }

    log_debug("Local attestation succeeded!");
    return 0;
}

int _PalStreamSecureInit(PAL_HANDLE stream, bool is_server, PAL_SESSION_KEY* session_key,
                         LIB_SSL_CONTEXT** out_ssl_ctx, const uint8_t* buf_load_ssl_ctx,
                         size_t buf_size) {
    int stream_fd;

    if (stream->hdr.type == PAL_TYPE_PROCESS)
        stream_fd = stream->process.stream;
    else if (stream->hdr.type == PAL_TYPE_PIPE || stream->hdr.type == PAL_TYPE_PIPECLI)
        stream_fd = stream->pipe.fd;
    else
        return -PAL_ERROR_BADHANDLE;

    LIB_SSL_CONTEXT* ssl_ctx = malloc(sizeof(*ssl_ctx));
    if (!ssl_ctx)
        return -PAL_ERROR_NOMEM;

    /* mbedTLS init routines are not thread safe, so we use a spinlock to protect them */
    static spinlock_t ssl_init_lock = INIT_SPINLOCK_UNLOCKED;

    spinlock_lock(&ssl_init_lock);
    int ret = lib_SSLInit(ssl_ctx, stream_fd, is_server,
                          (const uint8_t*)session_key, sizeof(*session_key),
                          ocall_read, ocall_write, buf_load_ssl_ctx, buf_size);
    spinlock_unlock(&ssl_init_lock);

    if (ret != 0) {
        free(ssl_ctx);
        return ret;
    }

    if (!buf_load_ssl_ctx) {
        /* TLS context was not restored from the buffer, need to perform handshake */
        ret = lib_SSLHandshake(ssl_ctx);
        if (ret != 0) {
            free(ssl_ctx);
            return ret;
        }
    }

    *out_ssl_ctx = ssl_ctx;
    return 0;
}

int _PalStreamSecureFree(LIB_SSL_CONTEXT* ssl_ctx) {
    lib_SSLFree(ssl_ctx);
    free(ssl_ctx);
    return 0;
}

int _PalStreamSecureRead(LIB_SSL_CONTEXT* ssl_ctx, uint8_t* buf, size_t len, bool is_blocking) {
    int ret = lib_SSLRead(ssl_ctx, buf, len);
    if (is_blocking && ret == -PAL_ERROR_TRYAGAIN) {
        /* mbedTLS wrappers collapse host errors `EAGAIN` and `EINTR` into one error PAL
         * (`PAL_ERROR_TRYAGAIN`). We use the fact that blocking reads do not return `EAGAIN` to
         * split it back. */
        return -PAL_ERROR_INTERRUPTED;
    }
    return ret;
}

int _PalStreamSecureWrite(LIB_SSL_CONTEXT* ssl_ctx, const uint8_t* buf, size_t len,
                          bool is_blocking) {
    int ret = lib_SSLWrite(ssl_ctx, buf, len);
    if (is_blocking && ret == -PAL_ERROR_TRYAGAIN) {
        /* See the explanation in `_PalStreamSecureRead`. */
        return -PAL_ERROR_INTERRUPTED;
    }
    return ret;
}

int _PalStreamSecureSave(LIB_SSL_CONTEXT* ssl_ctx, const uint8_t** obuf, size_t* olen) {
    assert(obuf);
    assert(olen);

    int ret;

    /* figure out the required buffer size */
    ret = lib_SSLSave(ssl_ctx, NULL, 0, olen);
    if (ret != 0 && ret != -PAL_ERROR_NOMEM)
        return ret;

    /* create the required buffer */
    size_t len   = *olen;
    uint8_t* buf = malloc(len);
    if (!buf)
        return -PAL_ERROR_NOMEM;

    /* now have buffer with sufficient size to save serialized context */
    ret = lib_SSLSave(ssl_ctx, buf, len, olen);
    if (ret != 0 || len != *olen) {
        free(buf);
        return -PAL_ERROR_DENIED;
    }

    *obuf = buf;
    return 0;
}
