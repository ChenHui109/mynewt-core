#include <assert.h>
#include <stddef.h>
#include <inttypes.h>
#include <string.h>
#include "hal/hal_flash.h"
#include "ffs/ffs.h"
#include "ffsutil/ffsutil.h"
#include "bootutil/loader.h"
#include "bootutil_priv.h"

static const struct boot_req *boot_req;
struct image_header boot_img_hdrs[2];
struct boot_status boot_status;
struct boot_status_entry boot_status_entries[64]; // XXX
const struct ffs_area_desc *boot_area_descs;

/**
 * Searches flash for an image with the specified version number.
 *
 * @param ver                   The version number to search for.
 *
 * @return                      The image slot containing the specified image
 *                              on success; -1 on failure.
 */
static int
boot_find_image_slot(const struct image_version *ver)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (memcmp(&boot_img_hdrs[i].ih_ver, ver, sizeof *ver) == 0) {
            return i;
        }
    }

    return -1;
}

/**
 * Selects a slot number to boot from, based on the contents of the boot
 * vector.
 *
 * @return                      The slot number to boot from on success;
 *                              -1 if an appropriate slot could not be
 *                              determined.
 */
static int
boot_select_image_slot(void)
{
    struct image_version ver;
    int slot;
    int rc;

    rc = boot_vect_read_test(&ver);
    if (rc == 0) {
        boot_vect_delete_test();
        slot = boot_find_image_slot(&ver);
        if (slot != -1) {
            return slot;
        }
    }

    rc = boot_vect_read_main(&ver);
    if (rc == 0) {
        slot = boot_find_image_slot(&ver);
        if (slot == -1) {
            boot_vect_delete_main();
        } else {
            return slot;
        }
    }

    return -1;
}

/**
 * Searches the current boot status for the specified image-num,part-num pair.
 *
 * @param image_num             The image number to search for.
 * @param part_num              The part number of the specified image to
 *                                  search for.
 *
 * @return                      The area index containing the specified image
 *                              part number;
 *                              -1 if the part number is not present in flash.
 */
static int
boot_find_image_part(int image_num, int part_num)
{
    int i;

    for (i = 0; i < boot_req->br_num_image_areas; i++) {
        if (boot_status_entries[i].bse_image_num == image_num &&
            boot_status_entries[i].bse_part_num == part_num) {

            return boot_req->br_image_areas[i];
        }
    }

    return -1;
}

static int
boot_slot_to_area_idx(int slot_num)
{
    int i;

    assert(slot_num >= 0 && slot_num < boot_req->br_num_slots);

    for (i = 0; boot_req->br_area_descs[i].fad_length != 0; i++) {
        if (boot_req->br_area_descs[i].fad_offset ==
            boot_req->br_image_addrs[slot_num]) {

            return i;
        }
    }

    return -1;
}

/**
 * Locates the specified area index within the array of image areas.
 *
 * @param area_idx            The area index to search for.
 *
 * @return                      The index of the element in boot_req->br_image_areas
 *                              that contains the sought after area index;
 *                              -1 if the area index is not present.
 */
static int
boot_find_image_area_idx(int area_idx)
{
    int i;

    for (i = 0; i < boot_req->br_num_image_areas; i++) {
        if (boot_req->br_image_areas[i] == area_idx) {
            return i;
        }
    }

    return -1;
}

static int
boot_erase_area(int area_idx)
{
    const struct ffs_area_desc *area_desc;
    int rc;

    area_desc = boot_req->br_area_descs + area_idx;
    rc = flash_erase(area_desc->fad_offset, area_desc->fad_length);
    if (rc != 0) {
        return BOOT_EFLASH;
    }

    return 0;
}

/**
 * Copies the contents of one area to another.  The destination area must
 * be erased prior to this function being called.
 *
 * @param from_area_idx       The index of the source area.
 * @param to_area_idx         The index of the destination area.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
boot_copy_area(int from_area_idx, int to_area_idx)
{
    const struct ffs_area_desc *from_area_desc;
    const struct ffs_area_desc *to_area_desc;
    uint32_t from_addr;
    uint32_t to_addr;
    uint32_t off;
    int chunk_sz;
    int rc;

    static uint8_t buf[1024];

    from_area_desc = boot_req->br_area_descs + from_area_idx;
    to_area_desc = boot_req->br_area_descs + to_area_idx;

    assert(to_area_desc->fad_length >= from_area_desc->fad_length);

    off = 0;
    while (off < from_area_desc->fad_length) {
        if (from_area_desc->fad_length - off > sizeof buf) {
            chunk_sz = sizeof buf;
        } else {
            chunk_sz = from_area_desc->fad_length - off;
        }

        from_addr = from_area_desc->fad_offset + off;
        rc = flash_read(buf, from_addr, chunk_sz);
        if (rc != 0) {
            return rc;
        }

        to_addr = to_area_desc->fad_offset + off;
        rc = flash_write(buf, to_addr, chunk_sz);
        if (rc != 0) {
            return rc;
        }

        off += chunk_sz;
    }

    return 0;
}

/**
 * Swaps the contents of two flash areas.
 *
 * @param area_idx_1          The index of one area to swap.  This area
 *                                  must be part of the first image slot.
 * @param part_num_1            The image part number stored in the first
 *                                  area.
 * @param area_idx_2          The index of the other area to swap.  This
 *                                  area must be part of the second image
 *                                  slot.
 * @param part_num_2            The image part number stored in the second
 *                                  area.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
boot_move_area(int from_area_idx, int to_area_idx,
                 int img_num, uint8_t part_num)
{
    int src_image_idx;
    int dst_image_idx;
    int rc;

    src_image_idx = boot_find_image_area_idx(from_area_idx);
    assert(src_image_idx != -1);

    dst_image_idx = boot_find_image_area_idx(to_area_idx);
    assert(dst_image_idx != -1);

    rc = boot_erase_area(to_area_idx);
    if (rc != 0) {
        return rc;
    }

    rc = boot_copy_area(from_area_idx, to_area_idx);
    if (rc != 0) {
        return rc;
    }

    boot_status_entries[src_image_idx].bse_image_num = BOOT_IMAGE_NUM_NONE;
    boot_status_entries[src_image_idx].bse_part_num = BOOT_IMAGE_NUM_NONE;
    boot_status_entries[dst_image_idx].bse_image_num = img_num;
    boot_status_entries[dst_image_idx].bse_part_num = part_num;
    rc = boot_write_status(&boot_status, boot_status_entries,
                           boot_req->br_num_image_areas);
    if (rc != 0) {
        return rc;
    }

    rc = boot_erase_area(from_area_idx);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/**
 * Swaps the contents of two flash areas.
 *
 * @param area_idx_1          The index of one area to swap.  This area
 *                                  must be part of the first image slot.
 * @param part_num_1            The image part number stored in the first
 *                                  area.
 * @param area_idx_2          The index of the other area to swap.  This
 *                                  area must be part of the second image
 *                                  slot.
 * @param part_num_2            The image part number stored in the second
 *                                  area.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
boot_swap_areas(int area_idx_1, int img_num_1, uint8_t part_num_1,
                  int area_idx_2, int img_num_2, uint8_t part_num_2)
{
    int scratch_image_idx;
    int image_idx_1;
    int image_idx_2;
    int rc;

    assert(area_idx_1 != area_idx_2);
    assert(area_idx_1 != boot_req->br_scratch_area_idx);
    assert(area_idx_2 != boot_req->br_scratch_area_idx);

    image_idx_1 = boot_find_image_area_idx(area_idx_1);
    assert(image_idx_1 != -1);

    image_idx_2 = boot_find_image_area_idx(area_idx_2);
    assert(image_idx_2 != -1);

    scratch_image_idx =
        boot_find_image_area_idx(boot_req->br_scratch_area_idx);
    assert(scratch_image_idx != -1);

    rc = boot_erase_area(boot_req->br_scratch_area_idx);
    if (rc != 0) {
        return rc;
    }

    rc = boot_copy_area(area_idx_2, boot_req->br_scratch_area_idx);
    if (rc != 0) {
        return rc;
    }

    boot_status_entries[scratch_image_idx] = boot_status_entries[image_idx_2];
    boot_status_entries[image_idx_2].bse_image_num = BOOT_IMAGE_NUM_NONE;
    boot_status_entries[image_idx_2].bse_part_num = BOOT_IMAGE_NUM_NONE;

    rc = boot_write_status(&boot_status, boot_status_entries,
                           boot_req->br_num_image_areas);
    if (rc != 0) {
        return rc;
    }

    rc = boot_erase_area(area_idx_2);
    if (rc != 0) {
        return rc;
    }

    rc = boot_copy_area(area_idx_1, area_idx_2);
    if (rc != 0) {
        return rc;
    }

    boot_status_entries[image_idx_2] = boot_status_entries[image_idx_1];
    boot_status_entries[image_idx_1].bse_image_num = BOOT_IMAGE_NUM_NONE;
    boot_status_entries[image_idx_1].bse_part_num = BOOT_IMAGE_NUM_NONE;
    rc = boot_write_status(&boot_status, boot_status_entries,
                           boot_req->br_num_image_areas);
    if (rc != 0) {
        return rc;
    }

    rc = boot_erase_area(area_idx_1);
    if (rc != 0) {
        return rc;
    }

    rc = boot_copy_area(boot_req->br_scratch_area_idx, area_idx_1);
    if (rc != 0) {
        return rc;
    }

    boot_status_entries[image_idx_1] = boot_status_entries[scratch_image_idx];
    boot_status_entries[scratch_image_idx].bse_image_num = BOOT_IMAGE_NUM_NONE;
    boot_status_entries[scratch_image_idx].bse_part_num = BOOT_IMAGE_NUM_NONE;
    rc = boot_write_status(&boot_status, boot_status_entries,
                           boot_req->br_num_image_areas);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
boot_fill_slot(int img_num, uint32_t img_length, int start_area_idx)
{
    const struct ffs_area_desc *area_desc;
    uint32_t off;
    int dst_image_area_idx;
    int src_area_idx;
    int dst_area_idx;
    int part_num;
    int rc;

    part_num = 0;
    off = 0;
    while (off < img_length) {
        /* Determine which area contains the current part of the image that
         * we want to boot.
         */
        src_area_idx = boot_find_image_part(img_num, part_num);
        if (src_area_idx == -1) {
            return BOOT_EBADIMAGE;
        }

        /* Determine which area we want to copy the source to. */
        dst_area_idx = start_area_idx + part_num;

        if (src_area_idx != dst_area_idx) {
            /* Determine what is currently in the destination area. */
            dst_image_area_idx = boot_find_image_area_idx(dst_area_idx);

            if (boot_status_entries[dst_image_area_idx].bse_image_num ==
                BOOT_IMAGE_NUM_NONE) {

                /* The destination doesn't contain anything useful; we don't
                 * need to back up its contents.
                 */
                rc = boot_move_area(src_area_idx, dst_area_idx,
                                      img_num, part_num);
            } else {
                /* Swap the two areas. */
                rc = boot_swap_areas(src_area_idx, img_num ^ 1, part_num,
                                       dst_area_idx, img_num, part_num);
            }
            if (rc != 0) {
                return rc;
            }
        }

        area_desc = boot_req->br_area_descs + dst_area_idx;
        off += area_desc->fad_length;

        part_num++;
    }

    return 0;
}

/**
 * Swaps the two images in flash.  If a prior copy operation was interrupted
 * by a system reset, this function completes that operation.
 *
 * @param img1_length           The length, in bytes, of the slot 1 image.
 * @param img2_length           The length, in bytes, of the slot 2 image.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
boot_copy_image(uint32_t img1_length, uint32_t img2_length)
{
    int rc;

    rc = boot_fill_slot(1, img2_length, boot_slot_to_area_idx(0));
    if (rc != 0) {
        return rc;
    }

    rc = boot_fill_slot(0, img1_length, boot_slot_to_area_idx(1));
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static void
boot_build_status_one(int image_num, uint32_t addr, uint32_t length)
{
    uint32_t offset;
    int area_idx;
    int part_num;
    int i;

    for (i = 0; i < boot_req->br_num_image_areas; i++) {
        area_idx = boot_req->br_image_areas[i];
        if (boot_req->br_area_descs[area_idx].fad_offset == addr) {
            break;
        }
    }

    assert(i < boot_req->br_num_image_areas);

    offset = 0;
    part_num = 0;
    while (offset < length) {
        assert(boot_status_entries[i].bse_image_num == 0xff);
        boot_status_entries[i].bse_image_num = image_num;
        boot_status_entries[i].bse_part_num = part_num;

        offset += boot_req->br_area_descs[area_idx].fad_length;
        part_num++;
        i++;
        area_idx++;
    }

    assert(i <= boot_req->br_num_image_areas);
}

/**
 * Builds a default boot status corresponding to all images being fully present
 * in their slots.
 */
static void
boot_build_status(void)
{
    memset(boot_status_entries, 0xff, sizeof boot_status_entries);

    if (boot_img_hdrs[0].ih_magic == IMAGE_MAGIC) {
        boot_status.bs_img1_length = boot_img_hdrs[0].ih_img_size;
        boot_build_status_one(0, boot_req->br_image_addrs[0],
                              boot_img_hdrs[0].ih_img_size);
    } else {
        boot_status.bs_img1_length = 0;
    }

    if (boot_img_hdrs[1].ih_magic == IMAGE_MAGIC) {
        boot_status.bs_img2_length = boot_img_hdrs[1].ih_img_size;
        boot_build_status_one(1, boot_req->br_image_addrs[1],
                              boot_img_hdrs[1].ih_img_size);
    } else {
        boot_status.bs_img2_length = 0;
    }
}

static void
boot_init_flash(void)
{
    int rc;

    rc = flash_init();
    assert(rc == 0);

    rc = ffs_init();
    assert(rc == 0);

    /* Look for an ffs file system in internal flash. */
    ffs_detect(boot_req->br_area_descs);

    /* Just make sure the boot directory exists. */
    ffs_mkdir("/boot");
}

/**
 * Prepares the booting process.  Based on the information provided in the
 * request object, this function moves images around in flash as appropriate,
 * and tells you what address to boot from.
 *
 * @param req                   Contains information about the flash layout.
 * @param rsp                   On success, indicates how booting should occur.
 *
 * @return                      0 on success; nonzero on failure.
 */
int
boot_go(const struct boot_req *req, struct boot_rsp *rsp)
{
    int slot;
    int rc;

    boot_req = req;

    boot_init_flash();

    /* Determine if an image copy operation was interrupted (i.e., the system
     * was reset before the boot loader could finish its task last time).
     */
    rc = boot_read_status(&boot_status, boot_status_entries,
                          boot_req->br_num_image_areas);
    if (rc == 0) {
        /* We are resuming an interrupted image copy. */
        rc = boot_copy_image(boot_status.bs_img1_length,
                             boot_status.bs_img2_length);

        /* We failed to put the images back together; there is really no
         * solution here.
         */
        assert(rc == 0);
    }

    boot_read_image_headers(boot_img_hdrs, boot_req->br_image_addrs, 2);
    boot_build_status();

    slot = boot_select_image_slot();
    if (slot == -1) {
        /* Either there is no image vector, or none of the requested images are
         * present.  Just try booting from the first image slot.
         */
        if (boot_img_hdrs[0].ih_magic != IMAGE_MAGIC_NONE) {
            slot = 0;
        } else if (boot_img_hdrs[1].ih_magic != IMAGE_MAGIC_NONE) {
            slot = 1;
        } else {
            /* No images present. */
            assert(0);
        }
    }

    switch (slot) {
    case 0:
        rsp->br_hdr = &boot_img_hdrs[0];
        break;

    case 1:
        /* The user wants to run the image in the secondary slot.  The contents
         * of this slot need to moved to the primary slot.
         */
        rc = boot_copy_image(boot_status.bs_img1_length,
                             boot_status.bs_img2_length);

        /* We failed to put the images back together; there is really no
         * solution here.
         */
        assert(rc == 0);

        rsp->br_hdr = &boot_img_hdrs[1];
        break;

    default:
        assert(0);
        break;
    }

    rsp->br_image_addr = boot_req->br_image_addrs[0];

    ffs_unlink(BOOT_PATH_STATUS);

    return 0;
}

