/* The ZIP wrapper Steam's cloud stores a file in.
 *
 * A cloud file is not the save itself: ClientFileDownload hands back a ZIP with
 * a single entry named "z" whose contents are the raw bytes, and the sha_file
 * Steam reports is the SHA-1 of those raw bytes. Uploads have to be wrapped the
 * same way (Cloud.BeginHTTPUpload takes the ZIP size, the raw size and that
 * SHA-1).
 *
 * zlib (portlibs on the Switch, the system one on PC) does the deflate work;
 * this file only writes and parses the little ZIP container around it.
 */
#ifndef ISAAC_NATIVE_ZIP_H
#define ISAAC_NATIVE_ZIP_H

#include <stddef.h>
#include <stdint.h>

/* Deflate `raw` into a one-entry ZIP ("z"). Returns the ZIP size, or 0 when
 * `cap` is too small or zlib fails. `raw_len` must fit the ZIP's 32-bit
 * fields, which is true by orders of magnitude for a save file. */
size_t isaac_zip_pack(void *out, size_t cap, const void *raw, size_t raw_len);

/* Inflate the first entry of a ZIP (Steam names it "z"). `*out_len` is the
 * uncompressed size; the caller can pass the expected size in `cap`. Returns 0
 * on success, -1 on a malformed archive, a CRC mismatch or a short buffer. */
int isaac_zip_unpack(const void *zip, size_t zip_len, void *out, size_t cap, size_t *out_len);

/* The name Steam gives the entry; exposed for the tests. */
#define ISAAC_ZIP_ENTRY "z"

/* 1 when the bytes start with a local file header. Steam stores a cloud file
 * compressed or as-is (file_size == raw_file_size in that case), so downloads
 * have to look before unwrapping: a save that is not a ZIP is its own content. */
int isaac_zip_looks_like_zip(const void *data, size_t len);

#endif
