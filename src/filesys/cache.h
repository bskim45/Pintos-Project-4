#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "devices/timer.h"
#include "threads/synch.h"

#define CACHE_MAX_SIZE 64

struct disk_cache {
    
    uint8_t block[BLOCK_SECTOR_SIZE]; //size 512B
    block_sector_t disk_sector;

    bool is_free;
    int open_cnt;
    bool accessed;
    bool dirty;
};

struct lock cache_lock;
struct disk_cache cache_array[64];

void init_entry(int idx);
void init_cache(void);
int get_cache_entry(block_sector_t disk_sector);
int get_free_entry(void);
int access_cache_entry(block_sector_t disk_sector, bool dirty);
int replace_cache_entry(block_sector_t disk_sector, bool dirty);
void func_periodic_writer(void *aux);
void write_back(bool clear);
void func_read_ahead(void *aux);
void ahead_reader(block_sector_t);

#endif 
