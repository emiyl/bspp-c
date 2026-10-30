#include "native.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if BSPP_ENABLE_VITA
#include "basis_bridge.h"

typedef struct {
    Work work;
    Slice source;
    bool modern;
    size_t page;
    Buffer pvr;
} VitaJob;

static void encode_page(void *context) {
    VitaJob *job = context;
    Image image = decode_image(job->source, job->modern);
    uint64_t size = ((uint64_t)image.width + 3) / 4 *
                    (((uint64_t)image.height + 3) / 4) * 8;
    if (size > UINT32_MAX - 52)
        fail("Vita texture page %zu exceeds 32-bit PVR record size", job->page);
    Buffer *pvr = &job->pvr;
    put32(pvr, UINT32_C(0x03525650), false);
    put32(pvr, 0, false);
    put64(pvr, 5); /* PVR v3: PVRTCII 4bpp. */
    put32(pvr, 0, false); /* Linear RGB, matching PVRTexTool's lRGB setting. */
    put32(pvr, 0, false); /* Unsigned byte normalized channels. */
    put32(pvr, image.height, false);
    put32(pvr, image.width, false);
    /* One depth slice, surface, face, and mip level; no metadata. */
    for (int i = 0; i < 4; ++i)
        put32(pvr, 1, false);
    put32(pvr, 0, false);
    pvr->data = resize_array(pvr->data, 52 + (size_t)size, 1);
    pvr->capacity = pvr->size = 52 + (size_t)size;
    char error[256];
    if (!bspp_encode_pvrtc2(image.pixels, image.width, image.height,
                           pvr->data + 52, (size_t)size, error, sizeof error))
        fail("Vita texture page %zu: %s", job->page, error);
    free_image(&image);
}

static void write_bytes(FILE *file, const void *data, size_t size) {
    if (size && fwrite(data, 1, size, file) != size)
        fail("cannot write Vita texture package: %s", strerror(errno));
}

static void store_page(VitaJob *job, WorkerPool *workers, FILE *file) {
    worker_wait(workers, &job->work);
    Buffer length = {0};
    put32(&length, job->pvr.size, false);
    write_bytes(file, length.data, length.size);
    write_bytes(file, job->pvr.data, job->pvr.size);
    free(length.data);
    free(job->pvr.data);
    if (bspp_verbose)
        printf("Compressed Vita texture page %zu (%zu bytes)\n",
               job->page, job->pvr.size);
    *job = (VitaJob){0};
}

void process_vita(const Options *o, const DataWin *dw, WorkerPool *workers) {
    if (!dw->texture_count)
        fail("no embedded texture pages found for Vita output");
    for (size_t i = 0; i < dw->texture_count; ++i)
        if (!dw->textures[i].size)
            fail("Vita texture page %zu has no embedded image data", i);
    FILE *storage = tmpfile();
    if (!storage)
        fail("cannot create Vita texture spool: %s", strerror(errno));
    Buffer count = {0};
    put32(&count, dw->texture_count, false);
    write_bytes(storage, count.data, count.size);
    free(count.data);

    size_t capacity = worker_pool_window(workers), head = 0, pending = 0;
    VitaJob *jobs = allocate(capacity, sizeof *jobs);
    for (size_t i = 0; i < dw->texture_count; ++i) {
        if (pending == capacity) {
            store_page(&jobs[head], workers, storage);
            head = (head + 1) % capacity;
            --pending;
        }
        VitaJob *job = &jobs[(head + pending) % capacity];
        job->source = dw->textures[i];
        job->modern = version_at_least(dw, 2022, 5, 0, 0);
        job->page = i;
        worker_submit(workers, &job->work, encode_page, job);
        ++pending;
    }
    while (pending) {
        store_page(&jobs[head], workers, storage);
        head = (head + 1) % capacity;
        --pending;
    }
    free(jobs);
    if (fseeko(storage, 0, SEEK_SET))
        fail("cannot rewind Vita texture spool: %s", strerror(errno));
    char *path = join_path(o->output, "textures.bin");
    FILE *file = fopen(path, "wb");
    if (!file)
        fail("cannot create '%s': %s", path, strerror(errno));
    uint8_t bytes[65536];
    size_t size;
    while ((size = fread(bytes, 1, sizeof bytes, storage)))
        write_bytes(file, bytes, size);
    if (ferror(storage))
        fail("cannot read Vita texture spool: %s", strerror(errno));
    if (fclose(storage))
        fail("cannot close Vita texture spool: %s", strerror(errno));
    if (fclose(file))
        fail("cannot close '%s': %s", path, strerror(errno));
    if (bspp_verbose)
        printf("Wrote %zu Vita texture pages to %s\n", dw->texture_count, path);
    free(path);
}
#else
void process_vita(const Options *o, const DataWin *dw, WorkerPool *workers) {
    (void)o;
    (void)dw;
    (void)workers;
    fail("Vita support was disabled at build time (configure with -DENABLE_VITA=ON)");
}
#endif
