#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

// Basic layout rules from the PDF instructions
#define BLOCK_SIZE        4096U
#define JOURNAL_BLOCK_IDX    1U
#define JOURNAL_BLOCKS      16U
#define INODE_BMAP_IDX     17U
#define DATA_BMAP_IDX      18U
#define INODE_START_IDX    19U
#define DATA_START_IDX     21U

#define JOURNAL_MAGIC 0x4A524E4CU // "JRNL" [cite: 50]
#define REC_DATA   1
#define REC_COMMIT 2

// 1. The Journal Header [cite: 51]
struct journal_header {
    uint32_t magic;       // Must be JOURNAL_MAGIC
    uint32_t nbytes_used; // How many bytes are currently in the journal
};

// 2. The Record Header (A "Post-it" for each change) [cite: 64]
struct rec_header {
    uint16_t type; // REC_DATA or REC_COMMIT
    uint16_t size; // Total size of this record in bytes
};

// 3. The Data Record (The actual block change) [cite: 75]
struct data_record {
    struct rec_header hdr;
    uint32_t block_no;     // Which block on the disk is changing
    uint8_t data[BLOCK_SIZE]; // The new 4096-byte content
};

// 4. The Commit Record (The "Finish" stamp) [cite: 90]
struct commit_record {
    struct rec_header hdr;
};

// --- DATA STRUCTURES FROM MKFS.C ---
struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;
    uint8_t  _pad[128 - 9 * 4];
};

struct inode {
    uint16_t type;   // 0=free, 1=file, 2=dir
    uint16_t links;
    uint32_t size;
    uint32_t direct[8];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t _pad[128 - (2 + 2 + 4 + 8 * 4 + 4 + 4)];
};

struct dirent {
    uint32_t inode;
    char name[28];
};
// Tool to read a specific block (0-84) into a buffer
void read_block(int fd, uint32_t block_no, void *buffer) {
    lseek(fd, block_no * BLOCK_SIZE, SEEK_SET);
    if (read(fd, buffer, BLOCK_SIZE) != BLOCK_SIZE) {
        perror("Error reading block");
        exit(1);
    }
}

// Tool to write a specific block (0-84) from a buffer
void write_block(int fd, uint32_t block_no, void *buffer) {
    lseek(fd, block_no * BLOCK_SIZE, SEEK_SET);
    if (write(fd, buffer, BLOCK_SIZE) != BLOCK_SIZE) {
        perror("Error writing block");
        exit(1);
    }
}
int main(int argc, char *argv[]) {
    // 1. Check if the user typed enough words
    if (argc < 2) {
        printf("Usage: ./journal create <name> OR ./journal install\n");
        return 1;
    }

    // 2. Open the disk image file
    int fd = open("vsfs.img", O_RDWR);
    if (fd < 0) {
        perror("Error opening vsfs.img");
        return 1;
    }

    // 3. Check what command the user typed
if (strcmp(argv[1], "create") == 0) {
        if (argc < 3) {
            printf("Error: Provide a name.\n");
            close(fd);
            return 1;
        }

        char *filename = argv[2];
        uint8_t buffer[BLOCK_SIZE];

        // 1. Read Journal Header
        struct journal_header jh;
        read_block(fd, JOURNAL_BLOCK_IDX, buffer);
        memcpy(&jh, buffer, sizeof(struct journal_header));
        if (jh.magic != JOURNAL_MAGIC) {
            jh.magic = JOURNAL_MAGIC;
            jh.nbytes_used = sizeof(struct journal_header);
        }

        // 2. Find Free Inode
        uint8_t inode_bitmap[BLOCK_SIZE];
        read_block(fd, INODE_BMAP_IDX, inode_bitmap);
        int free_inode = -1;
        for (int i = 0; i < 32; i++) {
            if (!(inode_bitmap[i / 8] & (1 << (i % 8)))) {
                free_inode = i;
                break;
            }
        }
        if (free_inode == -1) { printf("No free inodes!\n"); close(fd); return 1; }

        // 3. Find Free Directory Entry & Prepare Directory Block
        uint8_t dir_block[BLOCK_SIZE];
        read_block(fd, DATA_START_IDX, dir_block);
        struct dirent *entries = (struct dirent *)dir_block;
        int free_entry = -1;
        for (int i = 2; i < (int)(BLOCK_SIZE / sizeof(struct dirent)); i++) {
            if (entries[i].inode == 0) { 
                free_entry = i;
                break;
            }
        }
        
        // --- PREPARE DATA TO JOURNAL ---

        // A. Update Bitmap
        inode_bitmap[free_inode / 8] |= (1 << (free_inode % 8));

        // B. Update Inode Table (Both Inode 0 and your new Inode)
        uint8_t inode_table_block[BLOCK_SIZE];
        read_block(fd, INODE_START_IDX, inode_table_block);
        struct inode *inodes = (struct inode *)inode_table_block;

        // Setup the NEW file inode
        inodes[free_inode].type = 1;
        inodes[free_inode].links = 1;
        inodes[free_inode].size = 0;
        inodes[free_inode].mtime = (uint32_t)time(NULL);

        // IMPORTANT: Grow the ROOT directory size to include the new entry
        uint32_t new_dir_size = (free_entry + 1) * sizeof(struct dirent);
        if (new_dir_size > inodes[0].size) {
            inodes[0].size = new_dir_size;
            inodes[0].mtime = (uint32_t)time(NULL); // Update root's modified time
        }

        // C. Update Directory Entry
        entries[free_entry].inode = free_inode;
        strncpy(entries[free_entry].name, filename, 27);
        entries[free_entry].name[27] = '\0';

        // --- WRITE TO JOURNAL ---

        // Record 1: Bitmap
        struct data_record dr;
        dr.hdr.type = REC_DATA;
        dr.hdr.size = sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE;
        dr.block_no = INODE_BMAP_IDX;
        printf("DEBUG: Writing Record 1 to journal for block %u\n", dr.block_no);
        memcpy(dr.data, inode_bitmap, BLOCK_SIZE);
        lseek(fd, (JOURNAL_BLOCK_IDX * BLOCK_SIZE) + jh.nbytes_used, SEEK_SET);
        write(fd, &dr, dr.hdr.size);
        jh.nbytes_used += dr.hdr.size;

        // Record 2: Inode Table (Updated Root + New Inode)
        dr.block_no = INODE_START_IDX;
        memcpy(dr.data, inode_table_block, BLOCK_SIZE);
        lseek(fd, (JOURNAL_BLOCK_IDX * BLOCK_SIZE) + jh.nbytes_used, SEEK_SET);
        write(fd, &dr, dr.hdr.size);
        jh.nbytes_used += dr.hdr.size;

        // Record 3: Directory Block
        dr.block_no = DATA_START_IDX;
        memcpy(dr.data, dir_block, BLOCK_SIZE);
        lseek(fd, (JOURNAL_BLOCK_IDX * BLOCK_SIZE) + jh.nbytes_used, SEEK_SET);
        write(fd, &dr, dr.hdr.size);
        jh.nbytes_used += dr.hdr.size;

        // Record 4: Commit
        struct commit_record cr;
        cr.hdr.type = REC_COMMIT;
        cr.hdr.size = sizeof(struct rec_header);
        lseek(fd, (JOURNAL_BLOCK_IDX * BLOCK_SIZE) + jh.nbytes_used, SEEK_SET);
        write(fd, &cr, cr.hdr.size);
        jh.nbytes_used += cr.hdr.size;

        // Finalize Journal Header
        lseek(fd, JOURNAL_BLOCK_IDX * BLOCK_SIZE, SEEK_SET);
        write(fd, &jh, sizeof(struct journal_header));
        printf("Success! File '%s' created in journal.\n", filename);
    }

        else if (strcmp(argv[1], "install") == 0) {
        uint8_t buffer[BLOCK_SIZE];
        struct journal_header jh;

        // 1. Read the Journal Header to see what's inside
        read_block(fd, JOURNAL_BLOCK_IDX, buffer);
        memcpy(&jh, buffer, sizeof(struct journal_header));

        if (jh.magic != JOURNAL_MAGIC || jh.nbytes_used <= sizeof(struct journal_header)) {
            printf("Journal is empty or invalid. Nothing to install.\n");
            close(fd);
            return 0;
        }

        printf("Installing %u bytes from journal...\n", jh.nbytes_used);

        // 2. Start reading records after the header
        uint32_t current_pos = sizeof(struct journal_header);
        
        // We will store up to 10 DATA records until we see a COMMIT
        struct data_record pending_updates[10];
        int update_count = 0;

        while (current_pos < jh.nbytes_used) {
            struct rec_header rh;
            // Seek to the next record in the journal area
            lseek(fd, (JOURNAL_BLOCK_IDX * BLOCK_SIZE) + current_pos, SEEK_SET);
            read(fd, &rh, sizeof(struct rec_header));

            if (rh.type == REC_DATA) {
                // Read the whole DATA record
                lseek(fd, (JOURNAL_BLOCK_IDX * BLOCK_SIZE) + current_pos, SEEK_SET);
                read(fd, &pending_updates[update_count], rh.size);
                update_count++;
                current_pos += rh.size;
            } 
            else if (rh.type == REC_COMMIT) {
                // We found a COMMIT! Now we copy all pending data to "Home"
                printf("Found COMMIT record. Applying %d updates...\n", update_count);
                for (int i = 0; i < update_count; i++) {
                    write_block(fd, pending_updates[i].block_no, pending_updates[i].data);
                    printf("  - Block %u updated.\n", pending_updates[i].block_no);
                }
                update_count = 0; // Reset for the next transaction
                current_pos += rh.size;
            }
        }

        // 3. CLEAR THE JOURNAL (Reset the counter to empty)
        jh.nbytes_used = sizeof(struct journal_header);
        memset(buffer, 0, sizeof(buffer));
        memcpy(buffer, &jh, sizeof(jh));
        write_block(fd, JOURNAL_BLOCK_IDX, buffer);
        printf("Installation complete. Journal cleared.\n");

    }else {
        printf("Unknown command.\n");
    }

    close(fd);
    return 0;
}
