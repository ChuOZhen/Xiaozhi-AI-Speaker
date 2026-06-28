/*
 * Minimal cJSON stub implementation for esp-sr linking
 * These are empty stubs to satisfy the linker.
 * The actual JSON functionality is not used in this project.
 */

#include <stdlib.h>
#include <string.h>

extern "C" {

// Opaque cJSON structure
typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;

// Stub: Parse JSON string (returns NULL = error)
__attribute__((used)) cJSON *cJSON_Parse(const char *value) {
    (void)value;
    return NULL;
}

// Stub: Get object item (returns NULL = not found)
__attribute__((used)) cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string) {
    (void)object;
    (void)string;
    return NULL;
}

// Stub: Delete cJSON object (does nothing)
__attribute__((used)) void cJSON_Delete(cJSON *c) {
    (void)c;
}

// Stub: Get string value (returns empty string)
__attribute__((used)) const char *cJSON_GetStringValue(const cJSON *item) {
    (void)item;
    return "";
}

// Stub: Get number value (returns 0)
__attribute__((used)) double cJSON_GetNumberValue(const cJSON *item) {
    (void)item;
    return 0.0;
}

// Stub: Is object (returns 0 = false)
__attribute__((used)) int cJSON_IsObject(const cJSON *item) {
    (void)item;
    return 0;
}

// Stub: Is string (returns 0 = false)
__attribute__((used)) int cJSON_IsString(const cJSON *item) {
    (void)item;
    return 0;
}

// Stub: Is number (returns 0 = false)
__attribute__((used)) int cJSON_IsNumber(const cJSON *item) {
    (void)item;
    return 0;
}

} // extern "C"
