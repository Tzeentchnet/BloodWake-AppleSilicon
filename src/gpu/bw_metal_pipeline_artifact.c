#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include "bw_metal_pipeline_artifact.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const uint8_t s_magic[8] = { 'B', 'W', 'M', 'P', 'A', '0', '1', 0 };

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static int nonzero_digest(const uint8_t digest[BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES])
{
    uint8_t value = 0;
    size_t i;
    for (i = 0; i < BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES; i++)
        value |= digest[i];
    return value != 0;
}

static int valid_kind(BwMetalPipelineArtifactKind kind)
{
    return kind == BW_METAL_PIPELINE_ARTIFACT_TRACE_KEYS ||
           kind == BW_METAL_PIPELINE_ARTIFACT_BINARY_ARCHIVE;
}

static int valid_identity(const BwMetalPipelineArtifactIdentity *identity)
{
    return identity && identity->backend_abi && identity->pipeline_key_schema &&
           nonzero_digest(identity->stable_shader_sha256) &&
           nonzero_digest(identity->executable_sha256) &&
           identity->os_major && identity->sdk_version &&
           identity->metal_feature_level && identity->gpu_family;
}

static BwMetalPipelineArtifactStatus metadata_status(
    const BwMetalPipelineArtifactMetadata *metadata)
{
    if (!metadata || !valid_kind(metadata->kind) ||
        metadata->flags != BW_METAL_PIPELINE_ARTIFACT_DEVICE_LOCAL ||
        !valid_identity(&metadata->identity) || !metadata->payload_bytes ||
        !metadata->entry_count || !nonzero_digest(metadata->payload_sha256))
        return BW_METAL_PIPELINE_ARTIFACT_FORMAT;
    if (metadata->payload_bytes >
            BW_METAL_PIPELINE_ARTIFACT_MAX_BYTES - BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES ||
        metadata->entry_count > BW_METAL_PIPELINE_ARTIFACT_MAX_ENTRIES)
        return BW_METAL_PIPELINE_ARTIFACT_LIMIT;
    return BW_METAL_PIPELINE_ARTIFACT_OK;
}

BwMetalPipelineArtifactStatus bw_metal_pipeline_artifact_encode_header(
    const BwMetalPipelineArtifactMetadata *metadata,
    uint8_t *header,
    size_t header_size)
{
    BwMetalPipelineArtifactStatus status;
    if (!metadata || !header || header_size != BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES)
        return BW_METAL_PIPELINE_ARTIFACT_ARGUMENT;
    status = metadata_status(metadata);
    if (status != BW_METAL_PIPELINE_ARTIFACT_OK)
        return status;
    memset(header, 0, header_size);
    memcpy(header, s_magic, sizeof s_magic);
    put32(header + 8, BW_METAL_PIPELINE_ARTIFACT_FORMAT_VERSION);
    put32(header + 12, BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES);
    put32(header + 16, (uint32_t)metadata->kind);
    put32(header + 20, metadata->flags);
    put32(header + 24, metadata->identity.backend_abi);
    put32(header + 28, metadata->identity.pipeline_key_schema);
    memcpy(header + 32, metadata->identity.stable_shader_sha256,
           BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES);
    memcpy(header + 64, metadata->identity.executable_sha256,
           BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES);
    put32(header + 96, metadata->identity.os_major);
    put32(header + 100, metadata->identity.os_minor);
    put32(header + 104, metadata->identity.os_patch);
    put32(header + 108, metadata->identity.sdk_version);
    put32(header + 112, metadata->identity.metal_feature_level);
    put32(header + 116, metadata->identity.gpu_family);
    put64(header + 120, metadata->payload_bytes);
    put32(header + 128, metadata->entry_count);
    memcpy(header + 136, metadata->payload_sha256,
           BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES);
    return BW_METAL_PIPELINE_ARTIFACT_OK;
}

void bw_metal_pipeline_artifact_sha256(const void *bytes,
                                       size_t size,
                                       uint8_t digest[BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES])
{
    CC_SHA256_CTX context;
    const uint8_t *p = (const uint8_t *)bytes;
    CC_SHA256_Init(&context);
    while (size) {
        CC_LONG chunk = size > UINT32_MAX ? UINT32_MAX : (CC_LONG)size;
        CC_SHA256_Update(&context, p, chunk);
        p += chunk;
        size -= chunk;
    }
    CC_SHA256_Final(digest, &context);
}

static BwMetalPipelineArtifactStatus decode_header(
    const uint8_t header[BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES],
    BwMetalPipelineArtifactMetadata *metadata)
{
    if (memcmp(header, s_magic, sizeof s_magic) ||
        get32(header + 8) != BW_METAL_PIPELINE_ARTIFACT_FORMAT_VERSION ||
        get32(header + 12) != BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES ||
        get32(header + 132) || get64(header + 168))
        return BW_METAL_PIPELINE_ARTIFACT_FORMAT;
    memset(metadata, 0, sizeof *metadata);
    metadata->kind = (BwMetalPipelineArtifactKind)get32(header + 16);
    metadata->flags = get32(header + 20);
    metadata->identity.backend_abi = get32(header + 24);
    metadata->identity.pipeline_key_schema = get32(header + 28);
    memcpy(metadata->identity.stable_shader_sha256, header + 32,
           BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES);
    memcpy(metadata->identity.executable_sha256, header + 64,
           BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES);
    metadata->identity.os_major = get32(header + 96);
    metadata->identity.os_minor = get32(header + 100);
    metadata->identity.os_patch = get32(header + 104);
    metadata->identity.sdk_version = get32(header + 108);
    metadata->identity.metal_feature_level = get32(header + 112);
    metadata->identity.gpu_family = get32(header + 116);
    metadata->payload_bytes = get64(header + 120);
    metadata->entry_count = get32(header + 128);
    memcpy(metadata->payload_sha256, header + 136,
           BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES);
    return metadata_status(metadata);
}

int bw_metal_pipeline_artifact_path_is_valid(const char *path)
{
    static const char suffix[] = ".bwmpa";
    size_t length;
    if (!path || path[0] != '/')
        return 0;
    length = strnlen(path, PATH_MAX);
    return length < PATH_MAX && length > sizeof suffix - 1 &&
           !strcmp(path + length - (sizeof suffix - 1), suffix);
}

static int private_regular_file(int fd, struct stat *st)
{
    return fstat(fd, st) == 0 && S_ISREG(st->st_mode) &&
           st->st_uid == geteuid() && !(st->st_mode & 077u) && st->st_nlink == 1;
}

static int private_regular_state(const struct stat *st)
{
    return S_ISREG(st->st_mode) && st->st_uid == geteuid() &&
           !(st->st_mode & 077u) && st->st_nlink == 1;
}

static int read_exact_at(int fd, uint8_t *bytes, size_t size, off_t offset)
{
    while (size) {
        ssize_t count = pread(fd, bytes, size, offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return 0;
        bytes += (size_t)count;
        size -= (size_t)count;
        offset += count;
    }
    return 1;
}

static int same_file_state(const struct stat *before, const struct stat *after)
{
    return before->st_dev == after->st_dev &&
           before->st_ino == after->st_ino &&
           before->st_size == after->st_size &&
           before->st_mode == after->st_mode &&
           before->st_nlink == after->st_nlink &&
           before->st_mtimespec.tv_sec == after->st_mtimespec.tv_sec &&
           before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec &&
           before->st_ctimespec.tv_sec == after->st_ctimespec.tv_sec &&
           before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec;
}

static BwMetalPipelineArtifactStatus identity_status(
    const BwMetalPipelineArtifactIdentity *actual,
    const BwMetalPipelineArtifactIdentity *expected)
{
    if (actual->backend_abi != expected->backend_abi ||
        actual->pipeline_key_schema != expected->pipeline_key_schema ||
        actual->os_major != expected->os_major ||
        actual->os_minor != expected->os_minor ||
        actual->os_patch != expected->os_patch ||
        actual->sdk_version != expected->sdk_version ||
        actual->metal_feature_level != expected->metal_feature_level ||
        actual->gpu_family != expected->gpu_family)
        return BW_METAL_PIPELINE_ARTIFACT_INCOMPATIBLE;
    if (memcmp(actual->stable_shader_sha256, expected->stable_shader_sha256,
               BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES) ||
        memcmp(actual->executable_sha256, expected->executable_sha256,
               BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES))
        return BW_METAL_PIPELINE_ARTIFACT_STALE;
    return BW_METAL_PIPELINE_ARTIFACT_OK;
}

static BwMetalPipelineArtifactStatus payload_status(
    int fd,
    const BwMetalPipelineArtifactMetadata *metadata,
    uint8_t *payload,
    size_t payload_capacity)
{
    uint8_t buffer[16384];
    uint8_t digest[BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES];
    CC_SHA256_CTX context;
    uint64_t left = metadata->payload_bytes;
    off_t offset = BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES;
    size_t copied = 0;
    if (payload && metadata->payload_bytes > payload_capacity)
        return BW_METAL_PIPELINE_ARTIFACT_LIMIT;
    if (!CC_SHA256_Init(&context))
        return BW_METAL_PIPELINE_ARTIFACT_IO;
    while (left) {
        size_t chunk = left > sizeof buffer ? sizeof buffer : (size_t)left;
        if (!read_exact_at(fd, buffer, chunk, offset) ||
            !CC_SHA256_Update(&context, buffer, (CC_LONG)chunk))
            return BW_METAL_PIPELINE_ARTIFACT_IO;
        if (payload) {
            memcpy(payload + copied, buffer, chunk);
            copied += chunk;
        }
        left -= chunk;
        offset += (off_t)chunk;
    }
    if (!CC_SHA256_Final(digest, &context))
        return BW_METAL_PIPELINE_ARTIFACT_IO;
    return memcmp(digest, metadata->payload_sha256, sizeof digest)
        ? BW_METAL_PIPELINE_ARTIFACT_CORRUPT
        : BW_METAL_PIPELINE_ARTIFACT_OK;
}

BwMetalPipelineArtifactDisposition bw_metal_pipeline_artifact_disposition(
    BwMetalPipelineArtifactStatus status)
{
    return status == BW_METAL_PIPELINE_ARTIFACT_OK
        ? BW_METAL_PIPELINE_ARTIFACT_USE_ARTIFACT
        : BW_METAL_PIPELINE_ARTIFACT_COMPILE_RUNTIME;
}

static BwMetalPipelineArtifactResult read_path(
    const char *path,
    BwMetalPipelineArtifactKind expected_kind,
    const BwMetalPipelineArtifactIdentity *expected_identity,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_size)
{
    uint8_t header[BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES];
    BwMetalPipelineArtifactResult result;
    BwMetalPipelineArtifactMetadata metadata;
    BwMetalPipelineArtifactStatus status = BW_METAL_PIPELINE_ARTIFACT_IO;
    struct stat path_state, before, after;
    uint64_t expected_size;
    int fd = -1;
    memset(&result, 0, sizeof result);
    result.status = BW_METAL_PIPELINE_ARTIFACT_ARGUMENT;
    result.disposition = BW_METAL_PIPELINE_ARTIFACT_COMPILE_RUNTIME;
    if (payload_size)
        *payload_size = 0;
    if (!bw_metal_pipeline_artifact_path_is_valid(path) ||
        !valid_kind(expected_kind) ||
        !valid_identity(expected_identity))
        return result;
#if !defined(O_NOFOLLOW) || !defined(O_CLOEXEC)
    result.status = BW_METAL_PIPELINE_ARTIFACT_UNSUPPORTED;
    return result;
#else
    if (lstat(path, &path_state) != 0) {
        result.status = errno == ENOENT
            ? BW_METAL_PIPELINE_ARTIFACT_MISSING
            : BW_METAL_PIPELINE_ARTIFACT_IO;
        return result;
    }
    if (S_ISLNK(path_state.st_mode)) {
        result.status = BW_METAL_PIPELINE_ARTIFACT_SYMLINK;
        return result;
    }
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        result.status = errno == ELOOP
            ? BW_METAL_PIPELINE_ARTIFACT_SYMLINK
            : (errno == ENOENT ? BW_METAL_PIPELINE_ARTIFACT_MISSING
                               : BW_METAL_PIPELINE_ARTIFACT_IO);
        return result;
    }
    if (!private_regular_file(fd, &before)) {
        status = BW_METAL_PIPELINE_ARTIFACT_POLICY;
        goto out;
    }
    if ((uint64_t)before.st_size > BW_METAL_PIPELINE_ARTIFACT_MAX_BYTES) {
        status = BW_METAL_PIPELINE_ARTIFACT_LIMIT;
        goto out;
    }
    if (before.st_size < (off_t)BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES) {
        status = BW_METAL_PIPELINE_ARTIFACT_TRUNCATED;
        goto out;
    }
    if (!read_exact_at(fd, header, sizeof header, 0)) {
        status = BW_METAL_PIPELINE_ARTIFACT_IO;
        goto out;
    }
    status = decode_header(header, &metadata);
    if (status != BW_METAL_PIPELINE_ARTIFACT_OK)
        goto out;
    expected_size = BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES + metadata.payload_bytes;
    if ((uint64_t)before.st_size < expected_size) {
        status = BW_METAL_PIPELINE_ARTIFACT_TRUNCATED;
        goto out;
    }
    if ((uint64_t)before.st_size > expected_size) {
        status = BW_METAL_PIPELINE_ARTIFACT_TRAILING_DATA;
        goto out;
    }
    if (metadata.kind != expected_kind) {
        status = BW_METAL_PIPELINE_ARTIFACT_INCOMPATIBLE;
        goto out;
    }
    status = identity_status(&metadata.identity, expected_identity);
    if (status != BW_METAL_PIPELINE_ARTIFACT_OK)
        goto out;
    status = payload_status(fd, &metadata, payload, payload_capacity);
    if (status != BW_METAL_PIPELINE_ARTIFACT_OK)
        goto out;
    if (fstat(fd, &after) != 0 || !same_file_state(&before, &after)) {
        status = BW_METAL_PIPELINE_ARTIFACT_IO;
        goto out;
    }
    result.metadata = metadata;
out:
    if (close(fd) != 0 && status == BW_METAL_PIPELINE_ARTIFACT_OK) {
        status = BW_METAL_PIPELINE_ARTIFACT_IO;
        memset(&result.metadata, 0, sizeof result.metadata);
    }
    result.status = status;
    result.disposition = bw_metal_pipeline_artifact_disposition(status);
    if (status == BW_METAL_PIPELINE_ARTIFACT_OK && payload_size)
        *payload_size = (size_t)metadata.payload_bytes;
    return result;
#endif
}

BwMetalPipelineArtifactResult bw_metal_pipeline_artifact_validate_path(
    const char *path,
    BwMetalPipelineArtifactKind expected_kind,
    const BwMetalPipelineArtifactIdentity *expected_identity)
{
    return read_path(path, expected_kind, expected_identity, NULL, 0, NULL);
}

BwMetalPipelineArtifactResult bw_metal_pipeline_artifact_read_path(
    const char *path,
    BwMetalPipelineArtifactKind expected_kind,
    const BwMetalPipelineArtifactIdentity *expected_identity,
    void *payload,
    size_t payload_capacity,
    size_t *payload_size)
{
    BwMetalPipelineArtifactResult result;
    if (!payload || !payload_capacity || !payload_size) {
        memset(&result, 0, sizeof result);
        result.status = BW_METAL_PIPELINE_ARTIFACT_ARGUMENT;
        result.disposition = BW_METAL_PIPELINE_ARTIFACT_COMPILE_RUNTIME;
        if (payload_size)
            *payload_size = 0;
        return result;
    }
    return read_path(path, expected_kind, expected_identity,
                     payload, payload_capacity, payload_size);
}

BwMetalPipelineArtifactStatus bw_metal_pipeline_artifact_sha256_file(
    const char *path,
    uint8_t digest[BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES])
{
    uint8_t buffer[16384];
    CC_SHA256_CTX context;
    struct stat before, after;
    int fd;
    if (!path || !*path || !digest)
        return BW_METAL_PIPELINE_ARTIFACT_ARGUMENT;
#if !defined(O_NOFOLLOW) || !defined(O_CLOEXEC)
    return BW_METAL_PIPELINE_ARTIFACT_UNSUPPORTED;
#else
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return errno == ELOOP ? BW_METAL_PIPELINE_ARTIFACT_SYMLINK
                              : BW_METAL_PIPELINE_ARTIFACT_IO;
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        !CC_SHA256_Init(&context)) {
        close(fd);
        return BW_METAL_PIPELINE_ARTIFACT_IO;
    }
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 ||
            (count > 0 &&
             !CC_SHA256_Update(&context, buffer, (CC_LONG)count))) {
            close(fd);
            return BW_METAL_PIPELINE_ARTIFACT_IO;
        }
        if (!count)
            break;
    }
    if (!CC_SHA256_Final(digest, &context) ||
        fstat(fd, &after) != 0 || !same_file_state(&before, &after) ||
        close(fd) != 0)
        return BW_METAL_PIPELINE_ARTIFACT_IO;
    return BW_METAL_PIPELINE_ARTIFACT_OK;
#endif
}

static int write_exact(int fd, const uint8_t *bytes, size_t size)
{
    while (size) {
        ssize_t count = write(fd, bytes, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return 0;
        bytes += (size_t)count;
        size -= (size_t)count;
    }
    return 1;
}

BwMetalPipelineArtifactStatus bw_metal_pipeline_artifact_write_path(
    const char *path,
    BwMetalPipelineArtifactKind kind,
    const BwMetalPipelineArtifactIdentity *identity,
    const void *payload,
    size_t payload_bytes,
    uint32_t entry_count)
{
    BwMetalPipelineArtifactMetadata metadata;
    uint8_t header[BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES];
    char directory[PATH_MAX], basename[NAME_MAX + 1u];
    char temporary[NAME_MAX + 1u];
    const char *slash;
    struct stat directory_state, target_state, temporary_state;
    BwMetalPipelineArtifactStatus status;
    size_t directory_length, basename_length;
    int directory_fd = -1, fd = -1, temporary_created = 0;
    if (!bw_metal_pipeline_artifact_path_is_valid(path) ||
        !valid_kind(kind) || !valid_identity(identity) || !payload ||
        !payload_bytes || !entry_count)
        return BW_METAL_PIPELINE_ARTIFACT_ARGUMENT;
    if (payload_bytes >
            BW_METAL_PIPELINE_ARTIFACT_MAX_BYTES -
                BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES ||
        entry_count > BW_METAL_PIPELINE_ARTIFACT_MAX_ENTRIES)
        return BW_METAL_PIPELINE_ARTIFACT_LIMIT;
    slash = strrchr(path, '/');
    if (!slash || !slash[1])
        return BW_METAL_PIPELINE_ARTIFACT_ARGUMENT;
    directory_length = slash == path ? 1u : (size_t)(slash - path);
    basename_length = strlen(slash + 1);
    if (directory_length >= sizeof directory ||
        !basename_length || basename_length > NAME_MAX - 32u)
        return BW_METAL_PIPELINE_ARTIFACT_LIMIT;
    memcpy(directory, path, directory_length);
    directory[directory_length] = '\0';
    memcpy(basename, slash + 1, basename_length + 1u);
    memset(&metadata, 0, sizeof metadata);
    metadata.kind = kind;
    metadata.flags = BW_METAL_PIPELINE_ARTIFACT_DEVICE_LOCAL;
    metadata.identity = *identity;
    metadata.payload_bytes = payload_bytes;
    metadata.entry_count = entry_count;
    bw_metal_pipeline_artifact_sha256(payload, payload_bytes,
                                      metadata.payload_sha256);
    status = bw_metal_pipeline_artifact_encode_header(
        &metadata, header, sizeof header);
    if (status != BW_METAL_PIPELINE_ARTIFACT_OK)
        return status;
#if !defined(O_NOFOLLOW) || !defined(O_CLOEXEC) || !defined(O_DIRECTORY) || \
    !defined(AT_SYMLINK_NOFOLLOW)
    return BW_METAL_PIPELINE_ARTIFACT_UNSUPPORTED;
#else
    directory_fd = open(directory,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_fd < 0)
        return errno == ELOOP ? BW_METAL_PIPELINE_ARTIFACT_SYMLINK
                              : BW_METAL_PIPELINE_ARTIFACT_IO;
    if (fstat(directory_fd, &directory_state) != 0 ||
        !S_ISDIR(directory_state.st_mode) ||
        directory_state.st_uid != geteuid() ||
        (directory_state.st_mode & 077u)) {
        status = BW_METAL_PIPELINE_ARTIFACT_POLICY;
        goto out;
    }
    if (fstatat(directory_fd, basename, &target_state,
                AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(target_state.st_mode)) {
            status = BW_METAL_PIPELINE_ARTIFACT_SYMLINK;
            goto out;
        }
        if (!private_regular_state(&target_state)) {
            status = BW_METAL_PIPELINE_ARTIFACT_POLICY;
            goto out;
        }
    } else if (errno != ENOENT) {
        status = BW_METAL_PIPELINE_ARTIFACT_IO;
        goto out;
    }
    status = BW_METAL_PIPELINE_ARTIFACT_IO;
    for (unsigned attempt = 0; attempt < 16u; attempt++) {
        int written = snprintf(temporary, sizeof temporary,
                               "%s.tmp.%ld.%u", basename,
                               (long)getpid(), attempt);
        if (written < 0 || (size_t)written >= sizeof temporary) {
            status = BW_METAL_PIPELINE_ARTIFACT_LIMIT;
            goto out;
        }
        fd = openat(directory_fd, temporary,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    0600);
        if (fd >= 0) {
            temporary_created = 1;
            break;
        }
        if (errno != EEXIST) {
            status = errno == ELOOP ? BW_METAL_PIPELINE_ARTIFACT_SYMLINK
                                    : BW_METAL_PIPELINE_ARTIFACT_IO;
            goto out;
        }
    }
    if (fd < 0)
        goto out;
    if (fchmod(fd, 0600) != 0 ||
        !write_exact(fd, header, sizeof header) ||
        !write_exact(fd, payload, payload_bytes) ||
        fsync(fd) != 0 ||
        fstat(fd, &temporary_state) != 0 ||
        !private_regular_state(&temporary_state)) {
        status = BW_METAL_PIPELINE_ARTIFACT_IO;
        goto out;
    }
    if (close(fd) != 0) {
        fd = -1;
        status = BW_METAL_PIPELINE_ARTIFACT_IO;
        goto out;
    }
    fd = -1;
    if (renameat(directory_fd, temporary, directory_fd, basename) != 0 ||
        fsync(directory_fd) != 0) {
        status = BW_METAL_PIPELINE_ARTIFACT_IO;
        goto out;
    }
    temporary_created = 0;
    status = BW_METAL_PIPELINE_ARTIFACT_OK;
out:
    if (fd >= 0)
        close(fd);
    if (temporary_created)
        unlinkat(directory_fd, temporary, 0);
    if (directory_fd >= 0 && close(directory_fd) != 0 &&
        status == BW_METAL_PIPELINE_ARTIFACT_OK)
        status = BW_METAL_PIPELINE_ARTIFACT_IO;
    return status;
#endif
}

const char *bw_metal_pipeline_artifact_status_string(
    BwMetalPipelineArtifactStatus status)
{
    switch (status) {
    case BW_METAL_PIPELINE_ARTIFACT_OK: return "compatible";
    case BW_METAL_PIPELINE_ARTIFACT_ARGUMENT: return "invalid_argument";
    case BW_METAL_PIPELINE_ARTIFACT_UNSUPPORTED: return "unsupported_platform";
    case BW_METAL_PIPELINE_ARTIFACT_MISSING: return "missing";
    case BW_METAL_PIPELINE_ARTIFACT_SYMLINK: return "symlink_rejected";
    case BW_METAL_PIPELINE_ARTIFACT_POLICY: return "policy_rejected";
    case BW_METAL_PIPELINE_ARTIFACT_IO: return "io_error";
    case BW_METAL_PIPELINE_ARTIFACT_LIMIT: return "over_limit";
    case BW_METAL_PIPELINE_ARTIFACT_TRUNCATED: return "truncated";
    case BW_METAL_PIPELINE_ARTIFACT_TRAILING_DATA: return "trailing_data";
    case BW_METAL_PIPELINE_ARTIFACT_FORMAT: return "invalid_format";
    case BW_METAL_PIPELINE_ARTIFACT_STALE: return "stale";
    case BW_METAL_PIPELINE_ARTIFACT_INCOMPATIBLE: return "incompatible";
    case BW_METAL_PIPELINE_ARTIFACT_CORRUPT: return "corrupt";
    default: return "unknown_status";
    }
}
