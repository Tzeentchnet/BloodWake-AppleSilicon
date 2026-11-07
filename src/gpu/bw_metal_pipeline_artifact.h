#ifndef BW_METAL_PIPELINE_ARTIFACT_H
#define BW_METAL_PIPELINE_ARTIFACT_H

#include <stddef.h>
#include <stdint.h>

#define BW_METAL_PIPELINE_ARTIFACT_SCHEMA "bloodwake.metal-pipeline-artifact/v1"
#define BW_METAL_PIPELINE_ARTIFACT_FORMAT_VERSION 1u
#define BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES 176u
#define BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES 32u
#define BW_METAL_PIPELINE_ARTIFACT_MAX_BYTES (64u * 1024u * 1024u)
#define BW_METAL_PIPELINE_ARTIFACT_MAX_ENTRIES 4096u

#define BW_METAL_PIPELINE_BACKEND_ABI_VERSION 1u
#define BW_METAL_PIPELINE_KEY_SCHEMA_VERSION 1u

#define BW_METAL_PIPELINE_ARTIFACT_DEVICE_LOCAL 0x00000001u

typedef enum {
    BW_METAL_PIPELINE_ARTIFACT_TRACE_KEYS = 1,
    BW_METAL_PIPELINE_ARTIFACT_BINARY_ARCHIVE = 2
} BwMetalPipelineArtifactKind;

typedef struct {
    uint32_t backend_abi;
    uint32_t pipeline_key_schema;
    uint8_t stable_shader_sha256[BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES];
    uint8_t executable_sha256[BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES];
    uint32_t os_major;
    uint32_t os_minor;
    uint32_t os_patch;
    uint32_t sdk_version;
    uint32_t metal_feature_level;
    uint32_t gpu_family;
} BwMetalPipelineArtifactIdentity;

typedef struct {
    BwMetalPipelineArtifactKind kind;
    uint32_t flags;
    BwMetalPipelineArtifactIdentity identity;
    uint64_t payload_bytes;
    uint32_t entry_count;
    uint8_t payload_sha256[BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES];
} BwMetalPipelineArtifactMetadata;

typedef enum {
    BW_METAL_PIPELINE_ARTIFACT_OK = 0,
    BW_METAL_PIPELINE_ARTIFACT_ARGUMENT,
    BW_METAL_PIPELINE_ARTIFACT_UNSUPPORTED,
    BW_METAL_PIPELINE_ARTIFACT_MISSING,
    BW_METAL_PIPELINE_ARTIFACT_SYMLINK,
    BW_METAL_PIPELINE_ARTIFACT_POLICY,
    BW_METAL_PIPELINE_ARTIFACT_IO,
    BW_METAL_PIPELINE_ARTIFACT_LIMIT,
    BW_METAL_PIPELINE_ARTIFACT_TRUNCATED,
    BW_METAL_PIPELINE_ARTIFACT_TRAILING_DATA,
    BW_METAL_PIPELINE_ARTIFACT_FORMAT,
    BW_METAL_PIPELINE_ARTIFACT_STALE,
    BW_METAL_PIPELINE_ARTIFACT_INCOMPATIBLE,
    BW_METAL_PIPELINE_ARTIFACT_CORRUPT
} BwMetalPipelineArtifactStatus;

typedef enum {
    BW_METAL_PIPELINE_ARTIFACT_USE_ARTIFACT = 1,
    BW_METAL_PIPELINE_ARTIFACT_COMPILE_RUNTIME = 2
} BwMetalPipelineArtifactDisposition;

typedef struct {
    BwMetalPipelineArtifactStatus status;
    BwMetalPipelineArtifactDisposition disposition;
    BwMetalPipelineArtifactMetadata metadata;
} BwMetalPipelineArtifactResult;

/*
 * Encoding writes only the fixed compatibility envelope. It never writes a
 * payload or a file. Trace-derived payload production belongs to later work.
 */
BwMetalPipelineArtifactStatus bw_metal_pipeline_artifact_encode_header(
    const BwMetalPipelineArtifactMetadata *metadata,
    uint8_t *header,
    size_t header_size);

void bw_metal_pipeline_artifact_sha256(const void *bytes,
                                       size_t size,
                                       uint8_t digest[BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES]);

BwMetalPipelineArtifactStatus bw_metal_pipeline_artifact_sha256_file(
    const char *path,
    uint8_t digest[BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES]);

int bw_metal_pipeline_artifact_path_is_valid(const char *path);

/*
 * Validation never loads a Metal library or archive. Every rejection retains
 * an explicit status and requests runtime compilation; callers must report the
 * status rather than treating fallback as artifact success.
 */
BwMetalPipelineArtifactResult bw_metal_pipeline_artifact_validate_path(
    const char *path,
    BwMetalPipelineArtifactKind expected_kind,
    const BwMetalPipelineArtifactIdentity *expected_identity);

BwMetalPipelineArtifactResult bw_metal_pipeline_artifact_read_path(
    const char *path,
    BwMetalPipelineArtifactKind expected_kind,
    const BwMetalPipelineArtifactIdentity *expected_identity,
    void *payload,
    size_t payload_capacity,
    size_t *payload_size);

BwMetalPipelineArtifactStatus bw_metal_pipeline_artifact_write_path(
    const char *path,
    BwMetalPipelineArtifactKind kind,
    const BwMetalPipelineArtifactIdentity *identity,
    const void *payload,
    size_t payload_bytes,
    uint32_t entry_count);

BwMetalPipelineArtifactDisposition bw_metal_pipeline_artifact_disposition(
    BwMetalPipelineArtifactStatus status);
const char *bw_metal_pipeline_artifact_status_string(
    BwMetalPipelineArtifactStatus status);

#endif
