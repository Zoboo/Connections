#include "list.h"
#include <stdlib.h>
#include <string.h>

bool list_init(List *list, size_t item_size, int initial_capacity, ListFreeFunc free_func) {
    if (initial_capacity <= 0)
        initial_capacity = 4;

    list->items = malloc(item_size * initial_capacity);
    if (!list->items)
        return false;

    list->item_size = item_size;
    list->count = 0;
    list->capacity = initial_capacity;
    list->free_func = free_func;
    return true;
}

void list_clear(List *list) {
    if (list->free_func) {
        for (int i = 0; i < list->count; i++) {
            void *item = list_get(list, i);
            list->free_func(item);
        }
    }

    list->count = 0;
}

void list_free(List *list) {
    list_clear(list);

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    list->item_size = 0;
    list->free_func = NULL;
}

bool list_add(List *list, const void *item) {
    if (list->count >= list->capacity) {
        int new_capacity = list->capacity * 2;
        void *new_items = realloc(list->items, list->item_size * new_capacity);

        if (!new_items)
            return false;

        list->items = new_items;
        list->capacity = new_capacity;
    }

    void *dest = (char*)list->items + (list->count * list->item_size);
    memcpy(dest, item, list->item_size);
    list->count++;
    return true;
}

void *list_get(List *list, int index) {
    if (index < 0 || index >= list->count)
        return NULL;

    return (char*)list->items + (index * list->item_size);
}