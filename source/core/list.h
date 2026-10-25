#pragma once

#include <stddef.h>
#include <stdbool.h>

typedef void (*ListFreeFunc)(void *item);

typedef struct {
    void *items;
    size_t item_size;
    int count;
    int capacity;
    ListFreeFunc free_func;
} List;

bool list_init(List *list, size_t item_size, int initial_capacity, ListFreeFunc free_func);
void list_free(List *list);

bool list_add(List *list, const void *item);
void *list_get(List *list, int index);
void list_clear(List *list);