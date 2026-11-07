#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1

#include "bw_metal_pipeline_artifact.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static const uint8_t s_payload[] = {
    0x42, 0x57, 0x2d, 0x73, 0x79, 0x6e, 0x74, 0x68,
    0x65, 0x74, 0x69, 0x63, 0x2d, 0x61, 0x72, 0x74,
    0x69, 0x66, 0x61, 0x63, 0x74, 0x2d, 0x76, 0x31
};

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int write_all(int fd, const uint8_t *bytes, size_t size)
{
    while (size) {
        ssize_t count = write(fd, bytes, size);
        if (count <= 0)
            return 0;
        bytes += (size_t)count;
        size -= (size_t)count;
    }
    return 1;
}

static BwMetalPipelineArtifactMetadata sample_metadata(void)
{
    BwMetalPipelineArtifactMetadata metadata;
    size_t i;
    memset(&metadata, 0, sizeof metadata);
    metadata.kind = BW_METAL_PIPELINE_ARTIFACT_TRACE_KEYS;
    metadata.flags = BW_METAL_PIPELINE_ARTIFACT_DEVICE_LOCAL;
    metadata.identity.backend_abi = BW_METAL_PIPELINE_BACKEND_ABI_VERSION;
    metadata.identity.pipeline_key_schema = BW_METAL_PIPELINE_KEY_SCHEMA_VERSION;
    for (i = 0; i < BW_METAL_PIPELINE_ARTIFACT_DIGEST_BYTES; i++) {
        metadata.identity.stable_shader_sha256[i] = (uint8_t)(i + 1u);
        metadata.identity.executable_sha256[i] = (uint8_t)(0x80u + i);
    }
    metadata.identity.os_major = 15;
    metadata.identity.os_minor = 2;
    metadata.identity.os_patch = 1;
    metadata.identity.sdk_version = 150200;
    metadata.identity.metal_feature_level = 31;
    metadata.identity.gpu_family = 9;
    metadata.payload_bytes = sizeof s_payload;
    metadata.entry_count = 3;
    bw_metal_pipeline_artifact_sha256(s_payload, sizeof s_payload,
                                      metadata.payload_sha256);
    return metadata;
}

static int write_artifact(const char *path,
                          const uint8_t header[BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES],
                          size_t header_size,
                          const uint8_t *payload,
                          size_t payload_size,
                          const uint8_t *tail,
                          size_t tail_size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    int ok;
    if (fd < 0)
        return 0;
    ok = write_all(fd, header, header_size) &&
         write_all(fd, payload, payload_size) &&
         write_all(fd, tail, tail_size) &&
         close(fd) == 0;
    if (!ok)
        (void)close(fd);
    return ok;
}

static int encode_sample(uint8_t header[BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES],
                         BwMetalPipelineArtifactMetadata *metadata)
{
    *metadata = sample_metadata();
    return bw_metal_pipeline_artifact_encode_header(metadata, header,
             BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES) ==
           BW_METAL_PIPELINE_ARTIFACT_OK;
}

static BwMetalPipelineArtifactResult validate(
    const char *path,
    const BwMetalPipelineArtifactMetadata *metadata)
{
    return bw_metal_pipeline_artifact_validate_path(
        path, metadata->kind, &metadata->identity);
}

static int compatible_test(const char *path)
{
    uint8_t header[BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES];
    BwMetalPipelineArtifactMetadata metadata;
    BwMetalPipelineArtifactResult result;
    CHECK(encode_sample(header, &metadata));
    CHECK(!memcmp(header, "BWMPA01", 7));
    CHECK(header[8] == BW_METAL_PIPELINE_ARTIFACT_FORMAT_VERSION);
    CHECK(header[12] == BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES);
    CHECK(write_artifact(path, header, sizeof header, s_payload, sizeof s_payload,
                         NULL, 0));
    result = validate(path, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_OK);
    CHECK(result.disposition == BW_METAL_PIPELINE_ARTIFACT_USE_ARTIFACT);
    CHECK(result.metadata.payload_bytes == sizeof s_payload);
    CHECK(result.metadata.entry_count == metadata.entry_count);
    CHECK(!strcmp(bw_metal_pipeline_artifact_status_string(result.status),
                  "compatible"));
    CHECK(!strcmp(BW_METAL_PIPELINE_ARTIFACT_SCHEMA,
                  "bloodwake.metal-pipeline-artifact/v1"));
    CHECK(bw_metal_pipeline_artifact_encode_header(
              &metadata, header, sizeof header - 1) ==
          BW_METAL_PIPELINE_ARTIFACT_ARGUMENT);
    return 1;
}

static int stale_test(const char *path)
{
    uint8_t header[BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES];
    BwMetalPipelineArtifactMetadata metadata, expected;
    BwMetalPipelineArtifactResult result;
    CHECK(encode_sample(header, &metadata));
    CHECK(write_artifact(path, header, sizeof header, s_payload, sizeof s_payload,
                         NULL, 0));
    expected = metadata;
    expected.identity.stable_shader_sha256[0] ^= 1u;
    result = validate(path, &expected);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_STALE);
    CHECK(result.disposition == BW_METAL_PIPELINE_ARTIFACT_COMPILE_RUNTIME);
    CHECK(!strcmp(bw_metal_pipeline_artifact_status_string(result.status), "stale"));
    expected = metadata;
    expected.identity.executable_sha256[31] ^= 1u;
    result = validate(path, &expected);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_STALE);
    return 1;
}

static int incompatible_test(const char *path)
{
    uint8_t header[BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES];
    BwMetalPipelineArtifactMetadata metadata, expected;
    BwMetalPipelineArtifactIdentity *identity;
    BwMetalPipelineArtifactResult result;
    uint32_t *fields[8];
    size_t i;
    CHECK(encode_sample(header, &metadata));
    CHECK(write_artifact(path, header, sizeof header, s_payload, sizeof s_payload,
                         NULL, 0));
    expected = metadata;
    identity = &expected.identity;
    fields[0] = &identity->backend_abi;
    fields[1] = &identity->pipeline_key_schema;
    fields[2] = &identity->os_major;
    fields[3] = &identity->os_minor;
    fields[4] = &identity->os_patch;
    fields[5] = &identity->sdk_version;
    fields[6] = &identity->metal_feature_level;
    fields[7] = &identity->gpu_family;
    for (i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        *fields[i] += 1u;
        result = validate(path, &expected);
        CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_INCOMPATIBLE);
        *fields[i] -= 1u;
    }
    result = bw_metal_pipeline_artifact_validate_path(
        path, BW_METAL_PIPELINE_ARTIFACT_BINARY_ARCHIVE, &metadata.identity);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_INCOMPATIBLE);
    CHECK(!strcmp(bw_metal_pipeline_artifact_status_string(result.status),
                  "incompatible"));
    return 1;
}

static int bounds_test(const char *directory, const char *path)
{
    uint8_t header[BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES], tail = 0;
    BwMetalPipelineArtifactMetadata metadata, excessive;
    BwMetalPipelineArtifactResult result;
    char second[1024];
    int fd;
    CHECK(encode_sample(header, &metadata));
    CHECK(write_artifact(path, header, sizeof header - 1, NULL, 0, NULL, 0));
    result = validate(path, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_TRUNCATED);
    CHECK(unlink(path) == 0);

    CHECK(write_artifact(path, header, sizeof header, s_payload,
                         sizeof s_payload - 1, NULL, 0));
    result = validate(path, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_TRUNCATED);
    CHECK(unlink(path) == 0);

    CHECK(write_artifact(path, header, sizeof header, s_payload, sizeof s_payload,
                         &tail, 1));
    result = validate(path, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_TRAILING_DATA);
    CHECK(unlink(path) == 0);

    excessive = metadata;
    excessive.entry_count = BW_METAL_PIPELINE_ARTIFACT_MAX_ENTRIES + 1u;
    CHECK(bw_metal_pipeline_artifact_encode_header(
              &excessive, header, sizeof header) ==
          BW_METAL_PIPELINE_ARTIFACT_LIMIT);
    CHECK(encode_sample(header, &metadata));
    put32(header + 128, BW_METAL_PIPELINE_ARTIFACT_MAX_ENTRIES + 1u);
    CHECK(write_artifact(path, header, sizeof header, s_payload, sizeof s_payload,
                         NULL, 0));
    result = validate(path, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_LIMIT);
    CHECK(unlink(path) == 0);

    snprintf(second, sizeof second, "%s/large.bwmpa", directory);
    fd = open(second, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, (off_t)BW_METAL_PIPELINE_ARTIFACT_MAX_BYTES + 1) == 0);
    CHECK(close(fd) == 0);
    result = validate(second, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_LIMIT);
    CHECK(unlink(second) == 0);

    CHECK(encode_sample(header, &metadata));
    put32(header + 132, 1);
    CHECK(write_artifact(path, header, sizeof header, s_payload, sizeof s_payload,
                         NULL, 0));
    result = validate(path, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_FORMAT);
    CHECK(!strcmp(bw_metal_pipeline_artifact_status_string(
                      BW_METAL_PIPELINE_ARTIFACT_LIMIT), "over_limit"));
    return 1;
}

static int policy_test(const char *directory, const char *path)
{
    uint8_t header[BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES];
    BwMetalPipelineArtifactMetadata metadata;
    BwMetalPipelineArtifactResult result;
    char link_path[1024], hard_path[1024], fifo_path[1024], missing[1024];
    CHECK(encode_sample(header, &metadata));
    snprintf(missing, sizeof missing, "%s/missing.bwmpa", directory);
    result = validate(missing, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_MISSING);
    CHECK(result.disposition == BW_METAL_PIPELINE_ARTIFACT_COMPILE_RUNTIME);
    CHECK(write_artifact(path, header, sizeof header, s_payload, sizeof s_payload,
                         NULL, 0));

    snprintf(link_path, sizeof link_path, "%s/link.bwmpa", directory);
    CHECK(symlink(path, link_path) == 0);
    result = validate(link_path, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_SYMLINK);
    CHECK(unlink(link_path) == 0);

    snprintf(hard_path, sizeof hard_path, "%s/hard.bwmpa", directory);
    CHECK(link(path, hard_path) == 0);
    result = validate(path, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_POLICY);
    CHECK(unlink(hard_path) == 0);

    CHECK(chmod(path, 0644) == 0);
    result = validate(path, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_POLICY);
    CHECK(chmod(path, 0600) == 0);

    snprintf(fifo_path, sizeof fifo_path, "%s/fifo.bwmpa", directory);
    CHECK(mkfifo(fifo_path, 0600) == 0);
    result = validate(fifo_path, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_POLICY);
    CHECK(unlink(fifo_path) == 0);

    result = bw_metal_pipeline_artifact_validate_path(
        "relative.bwmpa", metadata.kind, &metadata.identity);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_ARGUMENT);
    result = bw_metal_pipeline_artifact_validate_path(
        path, (BwMetalPipelineArtifactKind)99, &metadata.identity);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_ARGUMENT);
    CHECK(!strcmp(bw_metal_pipeline_artifact_status_string(
                      BW_METAL_PIPELINE_ARTIFACT_SYMLINK), "symlink_rejected"));
    return 1;
}

static int corrupt_test(const char *path)
{
    uint8_t header[BW_METAL_PIPELINE_ARTIFACT_HEADER_BYTES];
    uint8_t payload[sizeof s_payload];
    BwMetalPipelineArtifactMetadata metadata;
    BwMetalPipelineArtifactResult result;
    CHECK(encode_sample(header, &metadata));
    memcpy(payload, s_payload, sizeof payload);
    payload[3] ^= 1u;
    CHECK(write_artifact(path, header, sizeof header, payload, sizeof payload,
                         NULL, 0));
    result = validate(path, &metadata);
    CHECK(result.status == BW_METAL_PIPELINE_ARTIFACT_CORRUPT);
    CHECK(result.disposition == BW_METAL_PIPELINE_ARTIFACT_COMPILE_RUNTIME);
    CHECK(!strcmp(bw_metal_pipeline_artifact_status_string(result.status), "corrupt"));
    CHECK(bw_metal_pipeline_artifact_disposition(
              BW_METAL_PIPELINE_ARTIFACT_IO) ==
          BW_METAL_PIPELINE_ARTIFACT_COMPILE_RUNTIME);
    return 1;
}

int main(int argc, char **argv)
{
    char directory[] = "/tmp/bw-metal-artifact-XXXXXX";
    char path[1024];
    int ok = 0;
    if (argc != 2 || !mkdtemp(directory)) {
        fprintf(stderr, "usage: %s <compatible|stale|incompatible|bounds|policy|corrupt>\n",
                argv[0]);
        return 2;
    }
    snprintf(path, sizeof path, "%s/sample.bwmpa", directory);
    if (!strcmp(argv[1], "compatible"))
        ok = compatible_test(path);
    else if (!strcmp(argv[1], "stale"))
        ok = stale_test(path);
    else if (!strcmp(argv[1], "incompatible"))
        ok = incompatible_test(path);
    else if (!strcmp(argv[1], "bounds"))
        ok = bounds_test(directory, path);
    else if (!strcmp(argv[1], "policy"))
        ok = policy_test(directory, path);
    else if (!strcmp(argv[1], "corrupt"))
        ok = corrupt_test(path);
    if (access(path, F_OK) == 0)
        (void)unlink(path);
    if (rmdir(directory) != 0)
        ok = 0;
    return ok ? 0 : 1;
}
