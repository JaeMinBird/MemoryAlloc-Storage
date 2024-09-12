#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "jbod.h"
#include "mdadm.h"
#include "net.h"

int is_mounted = 0;
int is_written = 0;

int mdadm_mount(void) {
  if (jbod_client_operation(JBOD_MOUNT, NULL) == 0) {
    is_mounted = 1;
    return 1;
  }
  return -1;
}

int mdadm_unmount(void) {
  if (jbod_client_operation(JBOD_UNMOUNT, NULL) == 0) {
    is_mounted = 0;
    return 1;
  }
  return -1;
}

// helper to return min num of 2 ints
int min (int a, int b) {
    return (a<b ? a:b);
}

uint32_t jbod_construct_opcode(int op, uint32_t disk, uint32_t block, uint32_t padding) {
    uint32_t opcode = (op & 0x3F);
    opcode |= (disk & 0xF) << 6;
    opcode |= (block & 0xFF) << 10;
    opcode |= (padding & 0x3FFF) << 18;
    return opcode;
}

int mdadm_read(uint32_t start_addr, uint32_t read_len, uint8_t *read_buf)  {
	// check if mounted
    if (is_mounted == 0) {
        return -1;
    }
    // validate buffer and length
    if (read_buf == NULL && read_len > 0) {
        return -1;
    }

    //checking if read is in limits
    if (read_len == 0) {
        return 0;
    }
    if (read_len > 1024) {
        return -1;
    }

    //check if read range is valid
    uint32_t last_possible_addr = JBOD_NUM_DISKS * JBOD_DISK_SIZE - 1;
    if (start_addr > last_possible_addr || start_addr + read_len - 1 > last_possible_addr) {
        return -1;
    }

    uint8_t temp_buf[JBOD_BLOCK_SIZE]; // temp buffer for block data
    uint32_t current_block = start_addr / JBOD_BLOCK_SIZE;
    uint32_t end_block = (start_addr + read_len - 1) / JBOD_BLOCK_SIZE;
    uint32_t offset_within_block = start_addr % JBOD_BLOCK_SIZE;

    int read = 0; // set total bytes read to 0
    while (current_block <= end_block) {
        uint32_t disk_id = current_block / JBOD_NUM_BLOCKS_PER_DISK;
        uint32_t block_id = current_block % JBOD_NUM_BLOCKS_PER_DISK;

        // exec operations to position and read from JBOD

        // check cache first
        int cache_result = cache_lookup(disk_id, block_id, temp_buf);
        if (cache_result != 1) { // If not in cache, perform disk operations
            if (jbod_client_operation(jbod_construct_opcode(JBOD_SEEK_TO_DISK, disk_id, 0, 0), NULL) != 0 ||
                jbod_client_operation(jbod_construct_opcode(JBOD_SEEK_TO_BLOCK, 0, block_id, 0), NULL) != 0 ||
                jbod_client_operation(jbod_construct_opcode(JBOD_READ_BLOCK, 0, 0, 0), temp_buf) != 0) {
                return -1;
            }
            cache_insert(disk_id, block_id, temp_buf); // Insert into cache after reading
        }


        // calculate total bytes to copy from curr block
        uint32_t bytes_to_copy = min(JBOD_BLOCK_SIZE - offset_within_block, read_len);
        memcpy(read_buf, temp_buf + offset_within_block, bytes_to_copy);
        read_buf += bytes_to_copy;
        read_len -= bytes_to_copy;
        read += bytes_to_copy;

        current_block++; // move to next block
        offset_within_block = 0; // reset offset for subsequent blocks
    }

    return read; // return total bytes read
}

int mdadm_write_permission(void) {
    if (jbod_client_operation(JBOD_WRITE_PERMISSION, NULL) == 0) {
        is_written = 1;
        return 1;
    }
    return -1;
}

int mdadm_revoke_write_permission(void) {
    if (jbod_client_operation(JBOD_REVOKE_WRITE_PERMISSION, NULL) == 0) {
        is_written = 0;
        return 1;
    }
    return -1;
}
int mdadm_write(uint32_t start_addr, uint32_t write_len, const uint8_t *write_buf) {
    if (!is_mounted || !is_written) {
        return -1;
    }
    if (write_buf == NULL && write_len > 0) {
        return -1;
    }
    if (write_len == 0) {
        return 0;
    }
    if (write_len > 1024) {
        return -1;
    }

    uint32_t last_possible_addr = JBOD_NUM_DISKS * JBOD_DISK_SIZE - 1;
    if (start_addr > last_possible_addr || start_addr + write_len - 1 > last_possible_addr) {
        return -1;
    }

    uint8_t temp_buf[JBOD_BLOCK_SIZE];
    uint32_t current_block = start_addr / JBOD_BLOCK_SIZE;
    uint32_t end_block = (start_addr + write_len - 1) / JBOD_BLOCK_SIZE;
    uint32_t offset_within_block = start_addr % JBOD_BLOCK_SIZE;

    int written = 0;
    while (current_block <= end_block) {
        uint32_t disk_id = current_block / JBOD_NUM_BLOCKS_PER_DISK;
        uint32_t block_id = current_block % JBOD_NUM_BLOCKS_PER_DISK;

        // check cache first
        int cache_result = cache_lookup(disk_id, block_id, temp_buf);

        // not found in cache, read from disk
        if (cache_result != 1) {
            if (jbod_client_operation(jbod_construct_opcode(JBOD_SEEK_TO_DISK, disk_id, 0, 0), NULL) != 0 ||
                jbod_client_operation(jbod_construct_opcode(JBOD_SEEK_TO_BLOCK, 0, block_id, 0), NULL) != 0 ||
                jbod_client_operation(jbod_construct_opcode(JBOD_READ_BLOCK, 0, 0, 0), temp_buf) != 0) {
                return -1;
            }
        }

        uint32_t bytes_to_copy = min(JBOD_BLOCK_SIZE - offset_within_block, write_len);
        memcpy(temp_buf + offset_within_block, write_buf, bytes_to_copy);

        // apply the write-through plolicy
        if (jbod_client_operation(jbod_construct_opcode(JBOD_SEEK_TO_DISK, disk_id, 0, 0), NULL) != 0 ||
            jbod_client_operation(jbod_construct_opcode(JBOD_SEEK_TO_BLOCK, 0, block_id, 0), NULL) != 0 ||
            jbod_client_operation(jbod_construct_opcode(JBOD_WRITE_BLOCK, 0, 0, 0), temp_buf) != 0) {
            return -1;
        }

        // update or insert the block into cache
        cache_update(disk_id, block_id, temp_buf);

        write_buf += bytes_to_copy;
        write_len -= bytes_to_copy;
        written += bytes_to_copy;

        current_block++;
        offset_within_block = 0;
    }

    return written;
}
