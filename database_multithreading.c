#define _WIN32_WINNT 0x0600
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <string.h>
#include <ctype.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#define MAX_RECORDS 100
#define MAX_DEPARTMENTS 100
#define USERS_DB_FILE "users.db"
#define DEPARTMENTS_DB_FILE "departments.db"
#define BTREE_T 3
#define MAX_SQL_QUERY_LEN 512
#define MAX_SQL_RESULT_LEN 32768
#define MAX_CACHE_ENTRIES 16

int ids[MAX_RECORDS];
char names[MAX_RECORDS][50];
char emails[MAX_RECORDS][50];
int ages[MAX_RECORDS];
int department_ids[MAX_RECORDS];
int record_count = 0;

int dept_ids[MAX_DEPARTMENTS];
char dept_names[MAX_DEPARTMENTS][50];
int dept_count = 0;

CRITICAL_SECTION db_critical_section;

#define PORT "8080"

typedef struct BTreeNode {
    int n;
    int leaf;
    int keys[2 * BTREE_T - 1];
    int values[2 * BTREE_T - 1];
    struct BTreeNode* children[2 * BTREE_T];
} BTreeNode;

BTreeNode* user_index_root = NULL;

typedef struct {
    char key[50];
    char value[100];
} Param;

typedef struct {
    int valid;
    char query[MAX_SQL_QUERY_LEN];
    char result[MAX_SQL_RESULT_LEN];
    unsigned long long stamp;
} QueryCacheEntry;

QueryCacheEntry query_cache[MAX_CACHE_ENTRIES];
unsigned long long cache_clock = 0;

void save_users_database();
void load_users_database();
void save_departments_database();
void load_departments_database();
void insert_record(int id, const char* name, const char* email, int age, int department_id);
void delete_record(int id);
void upsert_department(int id, const char* name);
void delete_department(int id);
int assign_department(int user_id, int department_id);
int find_department_index_by_id(int department_id);
int find_user_index_by_id(int id);

BTreeNode* btree_create_node(int leaf);
void btree_free(BTreeNode* node);
int btree_search_index(BTreeNode* root, int key);
void btree_split_child(BTreeNode* parent, int child_index);
void btree_insert_nonfull(BTreeNode* node, int key, int value);
void btree_insert(int key, int value);
void rebuild_user_index();
void trim_inplace(char* s);
void strip_trailing_semicolon(char* s);
int starts_with_ci(const char* text, const char* prefix);
int streq_ci(const char* a, const char* b);
void to_upper_copy(const char* src, char* dst, size_t dst_size);
int execute_sql_statement(const char* sql, char* result_json, size_t result_size, int* is_select, int* changed_data, char* error_msg, size_t error_size);
int cache_get(const char* query, char* out_result, size_t out_size);
void cache_put(const char* query, const char* result);
void cache_invalidate_all();

void trim_inplace(char* s) {
    size_t len;
    char* start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

void strip_trailing_semicolon(char* s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
    if (len > 0 && s[len - 1] == ';') {
        s[len - 1] = '\0';
    }
    trim_inplace(s);
}

int starts_with_ci(const char* text, const char* prefix) {
    while (*prefix) {
        if (toupper((unsigned char)*text) != toupper((unsigned char)*prefix)) {
            return 0;
        }
        text++;
        prefix++;
    }
    return 1;
}

int streq_ci(const char* a, const char* b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

void to_upper_copy(const char* src, char* dst, size_t dst_size) {
    size_t i = 0;
    if (dst_size == 0) return;
    while (src[i] && i < dst_size - 1) {
        dst[i] = (char)toupper((unsigned char)src[i]);
        i++;
    }
    dst[i] = '\0';
}

int cache_get(const char* query, char* out_result, size_t out_size) {
    unsigned long long newest_stamp = 0;
    int match = -1;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (query_cache[i].valid && strcmp(query_cache[i].query, query) == 0) {
            if (query_cache[i].stamp >= newest_stamp) {
                newest_stamp = query_cache[i].stamp;
                match = i;
            }
        }
    }
    if (match < 0) return 0;

    strncpy(out_result, query_cache[match].result, out_size - 1);
    out_result[out_size - 1] = '\0';
    query_cache[match].stamp = ++cache_clock;
    return 1;
}

void cache_put(const char* query, const char* result) {
    int target = -1;
    unsigned long long oldest_stamp = 0;

    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (!query_cache[i].valid) {
            target = i;
            break;
        }
    }

    if (target < 0) {
        oldest_stamp = query_cache[0].stamp;
        target = 0;
        for (int i = 1; i < MAX_CACHE_ENTRIES; i++) {
            if (query_cache[i].stamp < oldest_stamp) {
                oldest_stamp = query_cache[i].stamp;
                target = i;
            }
        }
    }

    query_cache[target].valid = 1;
    query_cache[target].stamp = ++cache_clock;
    strncpy(query_cache[target].query, query, sizeof(query_cache[target].query) - 1);
    query_cache[target].query[sizeof(query_cache[target].query) - 1] = '\0';
    strncpy(query_cache[target].result, result, sizeof(query_cache[target].result) - 1);
    query_cache[target].result[sizeof(query_cache[target].result) - 1] = '\0';
}

void cache_invalidate_all() {
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        query_cache[i].valid = 0;
        query_cache[i].query[0] = '\0';
        query_cache[i].result[0] = '\0';
        query_cache[i].stamp = 0;
    }
}

int execute_sql_statement(const char* sql, char* result_json, size_t result_size, int* is_select, int* changed_data, char* error_msg, size_t error_size) {
    char sql_local[MAX_SQL_QUERY_LEN];
    char sql_upper[MAX_SQL_QUERY_LEN];
    int parsed;

    *is_select = 0;
    *changed_data = 0;
    error_msg[0] = '\0';
    (void)result_size;

    strncpy(sql_local, sql, sizeof(sql_local) - 1);
    sql_local[sizeof(sql_local) - 1] = '\0';
    trim_inplace(sql_local);
    strip_trailing_semicolon(sql_local);
    to_upper_copy(sql_local, sql_upper, sizeof(sql_upper));

    if (streq_ci(sql_upper, "SELECT * FROM USERS")) {
        *is_select = 1;
        strcpy(result_json, "[");
        EnterCriticalSection(&db_critical_section);
        for (int i = 0; i < record_count; i++) {
            char row[256];
            sprintf(row, "{\"id\":%d,\"name\":\"%s\",\"email\":\"%s\",\"age\":%d,\"department_id\":%d}",
                ids[i], names[i], emails[i], ages[i], department_ids[i]);
            strcat(result_json, row);
            if (i < record_count - 1) strcat(result_json, ",");
        }
        LeaveCriticalSection(&db_critical_section);
        strcat(result_json, "]");
        return 1;
    }

    if (starts_with_ci(sql_upper, "SELECT * FROM USERS WHERE ID =")) {
        int id;
        *is_select = 1;
        parsed = sscanf(sql_upper, "SELECT * FROM USERS WHERE ID = %d", &id);
        if (parsed != 1) {
            strncpy(error_msg, "Invalid SELECT WHERE syntax", error_size - 1);
            error_msg[error_size - 1] = '\0';
            return 0;
        }
        EnterCriticalSection(&db_critical_section);
        int idx = find_user_index_by_id(id);
        if (idx >= 0) {
            sprintf(result_json, "{\"id\":%d,\"name\":\"%s\",\"email\":\"%s\",\"age\":%d,\"department_id\":%d}",
                ids[idx], names[idx], emails[idx], ages[idx], department_ids[idx]);
        } else {
            strcpy(result_json, "{\"error\":\"Not found\"}");
        }
        LeaveCriticalSection(&db_critical_section);
        return 1;
    }

    if (streq_ci(sql_upper, "SELECT * FROM DEPARTMENTS")) {
        *is_select = 1;
        strcpy(result_json, "[");
        EnterCriticalSection(&db_critical_section);
        for (int i = 0; i < dept_count; i++) {
            char row[128];
            sprintf(row, "{\"id\":%d,\"name\":\"%s\"}", dept_ids[i], dept_names[i]);
            strcat(result_json, row);
            if (i < dept_count - 1) strcat(result_json, ",");
        }
        LeaveCriticalSection(&db_critical_section);
        strcat(result_json, "]");
        return 1;
    }

    if (streq_ci(sql_upper, "SELECT * FROM USERS JOIN DEPARTMENTS ON USERS.DEPARTMENT_ID = DEPARTMENTS.ID") ||
        streq_ci(sql_upper, "SELECT * FROM USERS_DEPARTMENTS")) {
        *is_select = 1;
        strcpy(result_json, "[");
        EnterCriticalSection(&db_critical_section);
        for (int i = 0; i < record_count; i++) {
            int didx = find_department_index_by_id(department_ids[i]);
            const char* dname = (didx >= 0) ? dept_names[didx] : "UNASSIGNED";
            char row[320];
            sprintf(row,
                "{\"id\":%d,\"name\":\"%s\",\"email\":\"%s\",\"age\":%d,\"department_id\":%d,\"department_name\":\"%s\"}",
                ids[i], names[i], emails[i], ages[i], department_ids[i], dname);
            strcat(result_json, row);
            if (i < record_count - 1) strcat(result_json, ",");
        }
        LeaveCriticalSection(&db_critical_section);
        strcat(result_json, "]");
        return 1;
    }

    if (starts_with_ci(sql_upper, "INSERT INTO USERS VALUES")) {
        int id, age, department_id;
        char name[50], email[50];
        char* open_paren = strchr(sql_local, '(');
        if (!open_paren) {
            strncpy(error_msg, "Invalid INSERT users syntax", error_size - 1);
            error_msg[error_size - 1] = '\0';
            return 0;
        }
        parsed = sscanf(open_paren, "(%d,'%49[^']','%49[^']',%d,%d)", &id, name, email, &age, &department_id);
        if (parsed != 5) {
            strncpy(error_msg, "Expected: INSERT INTO users VALUES (id,'name','email',age,department_id)", error_size - 1);
            error_msg[error_size - 1] = '\0';
            return 0;
        }

        EnterCriticalSection(&db_critical_section);
        if (find_user_index_by_id(id) >= 0 || record_count >= MAX_RECORDS) {
            LeaveCriticalSection(&db_critical_section);
            strncpy(error_msg, "User id exists or users table full", error_size - 1);
            error_msg[error_size - 1] = '\0';
            return 0;
        }
        insert_record(id, name, email, age, department_id);
        LeaveCriticalSection(&db_critical_section);

        *changed_data = 1;
        strcpy(result_json, "{\"success\":true,\"message\":\"User inserted\"}");
        return 1;
    }

    if (starts_with_ci(sql_upper, "INSERT INTO DEPARTMENTS VALUES")) {
        int id;
        char name[50];
        char* open_paren = strchr(sql_local, '(');
        if (!open_paren) {
            strncpy(error_msg, "Invalid INSERT departments syntax", error_size - 1);
            error_msg[error_size - 1] = '\0';
            return 0;
        }
        parsed = sscanf(open_paren, "(%d,'%49[^']')", &id, name);
        if (parsed != 2) {
            strncpy(error_msg, "Expected: INSERT INTO departments VALUES (id,'name')", error_size - 1);
            error_msg[error_size - 1] = '\0';
            return 0;
        }

        upsert_department(id, name);
        *changed_data = 1;
        strcpy(result_json, "{\"success\":true,\"message\":\"Department saved\"}");
        return 1;
    }

    if (starts_with_ci(sql_upper, "UPDATE USERS SET DEPARTMENT_ID =")) {
        int department_id, user_id;
        parsed = sscanf(sql_upper, "UPDATE USERS SET DEPARTMENT_ID = %d WHERE ID = %d", &department_id, &user_id);
        if (parsed != 2) {
            strncpy(error_msg, "Expected: UPDATE users SET department_id = N WHERE id = M", error_size - 1);
            error_msg[error_size - 1] = '\0';
            return 0;
        }

        EnterCriticalSection(&db_critical_section);
        int uidx = find_user_index_by_id(user_id);
        int didx = find_department_index_by_id(department_id);
        if (uidx < 0 || didx < 0) {
            LeaveCriticalSection(&db_critical_section);
            strncpy(error_msg, "User or department not found", error_size - 1);
            error_msg[error_size - 1] = '\0';
            return 0;
        }

        department_ids[uidx] = department_id;
        save_users_database();
        LeaveCriticalSection(&db_critical_section);

        *changed_data = 1;
        strcpy(result_json, "{\"success\":true,\"message\":\"User updated\"}");
        return 1;
    }

    if (starts_with_ci(sql_upper, "DELETE FROM USERS WHERE ID =")) {
        int id;
        parsed = sscanf(sql_upper, "DELETE FROM USERS WHERE ID = %d", &id);
        if (parsed != 1) {
            strncpy(error_msg, "Expected: DELETE FROM users WHERE id = N", error_size - 1);
            error_msg[error_size - 1] = '\0';
            return 0;
        }

        EnterCriticalSection(&db_critical_section);
        if (find_user_index_by_id(id) < 0) {
            LeaveCriticalSection(&db_critical_section);
            strncpy(error_msg, "User not found", error_size - 1);
            error_msg[error_size - 1] = '\0';
            return 0;
        }
        delete_record(id);
        LeaveCriticalSection(&db_critical_section);

        *changed_data = 1;
        strcpy(result_json, "{\"success\":true,\"message\":\"User deleted\"}");
        return 1;
    }

    if (starts_with_ci(sql_upper, "DELETE FROM DEPARTMENTS WHERE ID =")) {
        int id;
        parsed = sscanf(sql_upper, "DELETE FROM DEPARTMENTS WHERE ID = %d", &id);
        if (parsed != 1) {
            strncpy(error_msg, "Expected: DELETE FROM departments WHERE id = N", error_size - 1);
            error_msg[error_size - 1] = '\0';
            return 0;
        }

        EnterCriticalSection(&db_critical_section);
        if (find_department_index_by_id(id) < 0) {
            LeaveCriticalSection(&db_critical_section);
            strncpy(error_msg, "Department not found", error_size - 1);
            error_msg[error_size - 1] = '\0';
            return 0;
        }
        delete_department(id);
        LeaveCriticalSection(&db_critical_section);

        *changed_data = 1;
        strcpy(result_json, "{\"success\":true,\"message\":\"Department deleted\"}");
        return 1;
    }

    strncpy(error_msg, "Unsupported SQL statement", error_size - 1);
    error_msg[error_size - 1] = '\0';
    return 0;
}

void url_decode(char* dst, const char* src) {
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3];
            hex[0] = src[1];
            hex[1] = src[2];
            hex[2] = '\0';
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

int parse_query(char* query, Param* params, int max_params) {
    int count = 0;
    char* token = strtok(query, "&");
    while (token && count < max_params) {
        char* eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            strncpy(params[count].key, token, sizeof(params[count].key) - 1);
            params[count].key[sizeof(params[count].key) - 1] = '\0';
            url_decode(params[count].value, eq + 1);
            count++;
        }
        token = strtok(NULL, "&");
    }
    return count;
}

char* handle_request(char* request) {
    static char response[32768];
    char method[10], path[100], version[10];
    sscanf(request, "%s %s %s", method, path, version);

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/select") == 0) {
            strcpy(response, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n[");
            EnterCriticalSection(&db_critical_section);
            for (int i = 0; i < record_count; i++) {
                char buf[256];
                sprintf(buf, "{\"id\":%d,\"name\":\"%s\",\"email\":\"%s\",\"age\":%d,\"department_id\":%d}", ids[i], names[i], emails[i], ages[i], department_ids[i]);
                strcat(response, buf);
                if (i < record_count - 1) strcat(response, ",");
            }
            LeaveCriticalSection(&db_critical_section);
            strcat(response, "]");
        } else if (strcmp(path, "/departments") == 0) {
            strcpy(response, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n[");
            EnterCriticalSection(&db_critical_section);
            for (int i = 0; i < dept_count; i++) {
                char buf[128];
                sprintf(buf, "{\"id\":%d,\"name\":\"%s\"}", dept_ids[i], dept_names[i]);
                strcat(response, buf);
                if (i < dept_count - 1) strcat(response, ",");
            }
            LeaveCriticalSection(&db_critical_section);
            strcat(response, "]");
        } else if (strcmp(path, "/join/users-departments") == 0) {
            strcpy(response, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n[");
            EnterCriticalSection(&db_critical_section);
            for (int i = 0; i < record_count; i++) {
                int dep_idx = find_department_index_by_id(department_ids[i]);
                const char* dep_name = (dep_idx >= 0) ? dept_names[dep_idx] : "UNASSIGNED";
                char buf[320];
                sprintf(buf,
                    "{\"id\":%d,\"name\":\"%s\",\"email\":\"%s\",\"age\":%d,\"department_id\":%d,\"department_name\":\"%s\"}",
                    ids[i], names[i], emails[i], ages[i], department_ids[i], dep_name);
                strcat(response, buf);
                if (i < record_count - 1) strcat(response, ",");
            }
            LeaveCriticalSection(&db_critical_section);
            strcat(response, "]");
        } else if (strncmp(path, "/select/", 8) == 0) {
            int id = atoi(path + 8);
            EnterCriticalSection(&db_critical_section);
            int idx = find_user_index_by_id(id);
            if (idx >= 0) {
                int dep_idx = find_department_index_by_id(department_ids[idx]);
                const char* dep_name = (dep_idx >= 0) ? dept_names[dep_idx] : "UNASSIGNED";
                sprintf(response,
                    "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"id\":%d,\"name\":\"%s\",\"email\":\"%s\",\"age\":%d,\"department_id\":%d,\"department_name\":\"%s\"}",
                    ids[idx], names[idx], emails[idx], ages[idx], department_ids[idx], dep_name);
            } else {
                strcpy(response, "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"error\":\"Not found\"}");
            }
            LeaveCriticalSection(&db_critical_section);
        } else {
            strcpy(response, "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/sql") == 0) {
        char* cl = strstr(request, "Content-Length:");
        if (cl) {
            char* body = strstr(request, "\r\n\r\n") + 4;
            Param params[10];
            int n = parse_query(body, params, 10);
            char sql_query[MAX_SQL_QUERY_LEN] = "";

            for (int i = 0; i < n; i++) {
                if (strcmp(params[i].key, "query") == 0) {
                    strncpy(sql_query, params[i].value, sizeof(sql_query) - 1);
                    sql_query[sizeof(sql_query) - 1] = '\0';
                }
            }

            if (!sql_query[0]) {
                strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"message\":\"Missing query\"}");
            } else {
                char sql_result[MAX_SQL_RESULT_LEN];
                char err[256];
                int is_select = 0;
                int changed_data = 0;

                trim_inplace(sql_query);
                strip_trailing_semicolon(sql_query);

                EnterCriticalSection(&db_critical_section);
                if (starts_with_ci(sql_query, "SELECT")) {
                    if (cache_get(sql_query, sql_result, sizeof(sql_result))) {
                        LeaveCriticalSection(&db_critical_section);
                        sprintf(response,
                            "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":true,\"cached\":true,\"result\":%s}",
                            sql_result);
                        return response;
                    }
                }
                LeaveCriticalSection(&db_critical_section);

                if (execute_sql_statement(sql_query, sql_result, sizeof(sql_result), &is_select, &changed_data, err, sizeof(err))) {
                    EnterCriticalSection(&db_critical_section);
                    if (is_select) {
                        cache_put(sql_query, sql_result);
                    }
                    if (changed_data) {
                        cache_invalidate_all();
                    }
                    LeaveCriticalSection(&db_critical_section);

                    sprintf(response,
                        "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":true,\"cached\":false,\"result\":%s}",
                        sql_result);
                } else {
                    sprintf(response,
                        "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"message\":\"%s\"}",
                        err);
                }
            }
        } else {
            strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/insert") == 0) {
        char* cl = strstr(request, "Content-Length:");
        if (cl) {
            char* body = strstr(request, "\r\n\r\n") + 4;
            Param params[10];
            int n = parse_query(body, params, 10);
            int id = -1, age = -1, department_id = -1;
            char name[50] = "", email[50] = "";
            for (int i = 0; i < n; i++) {
                if (strcmp(params[i].key, "id") == 0) id = atoi(params[i].value);
                else if (strcmp(params[i].key, "name") == 0) strcpy(name, params[i].value);
                else if (strcmp(params[i].key, "email") == 0) strcpy(email, params[i].value);
                else if (strcmp(params[i].key, "age") == 0) age = atoi(params[i].value);
                else if (strcmp(params[i].key, "department_id") == 0) department_id = atoi(params[i].value);
            }
            if (id != -1 && name[0] && email[0] && age != -1) {
                EnterCriticalSection(&db_critical_section);
                int exists = (find_user_index_by_id(id) >= 0);
                if (!exists && record_count < MAX_RECORDS) {
                    insert_record(id, name, email, age, department_id);
                    strcpy(response, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":true,\"message\":\"Inserted\"}");
                } else {
                    strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"message\":\"ID exists or full\"}");
                }
                LeaveCriticalSection(&db_critical_section);
            } else {
                strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
            }
        } else {
            strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/insert-department") == 0) {
        char* cl = strstr(request, "Content-Length:");
        if (cl) {
            char* body = strstr(request, "\r\n\r\n") + 4;
            Param params[10];
            int n = parse_query(body, params, 10);
            int id = -1;
            char name[50] = "";
            for (int i = 0; i < n; i++) {
                if (strcmp(params[i].key, "id") == 0) id = atoi(params[i].value);
                else if (strcmp(params[i].key, "name") == 0) strcpy(name, params[i].value);
            }
            if (id != -1 && name[0]) {
                upsert_department(id, name);
                strcpy(response, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":true,\"message\":\"Department saved\"}");
            } else {
                strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"message\":\"Missing fields\"}");
            }
        } else {
            strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/assign-department") == 0) {
        char* cl = strstr(request, "Content-Length:");
        if (cl) {
            char* body = strstr(request, "\r\n\r\n") + 4;
            Param params[10];
            int n = parse_query(body, params, 10);
            int user_id = -1;
            int department_id = -1;
            for (int i = 0; i < n; i++) {
                if (strcmp(params[i].key, "user_id") == 0) user_id = atoi(params[i].value);
                else if (strcmp(params[i].key, "department_id") == 0) department_id = atoi(params[i].value);
            }

            if (user_id != -1 && department_id != -1) {
                int ok = assign_department(user_id, department_id);
                if (ok) {
                    strcpy(response, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":true,\"message\":\"Assignment updated\"}");
                } else {
                    strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"message\":\"User or department not found\"}");
                }
            } else {
                strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"message\":\"Missing fields\"}");
            }
        } else {
            strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        }
    } else if (strcmp(method, "DELETE") == 0 && strncmp(path, "/delete/", 8) == 0) {
        int id = atoi(path + 8);
        int found = 0;
        EnterCriticalSection(&db_critical_section);
        if (find_user_index_by_id(id) >= 0) {
            delete_record(id);
            found = 1;
        }
        LeaveCriticalSection(&db_critical_section);
        if (found) {
            strcpy(response, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":true,\"message\":\"Deleted\"}");
        } else {
            strcpy(response, "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"message\":\"Not found\"}");
        }
    } else if (strcmp(method, "DELETE") == 0 && strncmp(path, "/delete-department/", 19) == 0) {
        int id = atoi(path + 19);
        int found = 0;
        EnterCriticalSection(&db_critical_section);
        int idx = find_department_index_by_id(id);
        if (idx >= 0) {
            delete_department(id);
            found = 1;
        }
        LeaveCriticalSection(&db_critical_section);
        if (found) {
            strcpy(response, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":true,\"message\":\"Department deleted\"}");
        } else {
            strcpy(response, "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"message\":\"Department not found\"}");
        }
    } else {
        strcpy(response, "HTTP/1.1 405 Method Not Allowed\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
    }
    return response;
}

BTreeNode* btree_create_node(int leaf) {
    BTreeNode* node = (BTreeNode*)malloc(sizeof(BTreeNode));
    if (!node) return NULL;
    node->n = 0;
    node->leaf = leaf;
    for (int i = 0; i < 2 * BTREE_T; i++) {
        node->children[i] = NULL;
    }
    return node;
}

void btree_free(BTreeNode* node) {
    if (!node) return;
    if (!node->leaf) {
        for (int i = 0; i <= node->n; i++) {
            btree_free(node->children[i]);
        }
    }
    free(node);
}

int btree_search_index(BTreeNode* root, int key) {
    if (!root) return -1;
    int i = 0;
    while (i < root->n && key > root->keys[i]) {
        i++;
    }
    if (i < root->n && key == root->keys[i]) {
        return root->values[i];
    }
    if (root->leaf) {
        return -1;
    }
    return btree_search_index(root->children[i], key);
}

void btree_split_child(BTreeNode* parent, int child_index) {
    BTreeNode* full = parent->children[child_index];
    BTreeNode* right = btree_create_node(full->leaf);
    if (!right) return;

    right->n = BTREE_T - 1;
    for (int j = 0; j < BTREE_T - 1; j++) {
        right->keys[j] = full->keys[j + BTREE_T];
        right->values[j] = full->values[j + BTREE_T];
    }

    if (!full->leaf) {
        for (int j = 0; j < BTREE_T; j++) {
            right->children[j] = full->children[j + BTREE_T];
        }
    }

    full->n = BTREE_T - 1;

    for (int j = parent->n; j >= child_index + 1; j--) {
        parent->children[j + 1] = parent->children[j];
    }
    parent->children[child_index + 1] = right;

    for (int j = parent->n - 1; j >= child_index; j--) {
        parent->keys[j + 1] = parent->keys[j];
        parent->values[j + 1] = parent->values[j];
    }

    parent->keys[child_index] = full->keys[BTREE_T - 1];
    parent->values[child_index] = full->values[BTREE_T - 1];
    parent->n += 1;
}

void btree_insert_nonfull(BTreeNode* node, int key, int value) {
    int i = node->n - 1;
    if (node->leaf) {
        while (i >= 0 && key < node->keys[i]) {
            node->keys[i + 1] = node->keys[i];
            node->values[i + 1] = node->values[i];
            i--;
        }
        node->keys[i + 1] = key;
        node->values[i + 1] = value;
        node->n += 1;
    } else {
        while (i >= 0 && key < node->keys[i]) {
            i--;
        }
        i++;
        if (node->children[i]->n == 2 * BTREE_T - 1) {
            btree_split_child(node, i);
            if (key > node->keys[i]) {
                i++;
            }
        }
        btree_insert_nonfull(node->children[i], key, value);
    }
}

void btree_insert(int key, int value) {
    if (!user_index_root) {
        user_index_root = btree_create_node(1);
    }

    if (user_index_root->n == 2 * BTREE_T - 1) {
        BTreeNode* new_root = btree_create_node(0);
        if (!new_root) return;
        new_root->children[0] = user_index_root;
        btree_split_child(new_root, 0);

        int i = 0;
        if (new_root->keys[0] < key) {
            i++;
        }
        btree_insert_nonfull(new_root->children[i], key, value);
        user_index_root = new_root;
    } else {
        btree_insert_nonfull(user_index_root, key, value);
    }
}

void rebuild_user_index() {
    btree_free(user_index_root);
    user_index_root = btree_create_node(1);
    if (!user_index_root) return;

    for (int i = 0; i < record_count; i++) {
        btree_insert(ids[i], i);
    }
}

int find_user_index_by_id(int id) {
    return btree_search_index(user_index_root, id);
}

int find_department_index_by_id(int department_id) {
    for (int i = 0; i < dept_count; i++) {
        if (dept_ids[i] == department_id) {
            return i;
        }
    }
    return -1;
}

void save_users_database() {
    FILE* file = fopen(USERS_DB_FILE, "wb");
    if (file == NULL) {
        return;
    }
    fwrite(&record_count, sizeof(int), 1, file);
    fwrite(ids, sizeof(int), record_count, file);
    fwrite(names, sizeof(char[50]), record_count, file);
    fwrite(emails, sizeof(char[50]), record_count, file);
    fwrite(ages, sizeof(int), record_count, file);
    fwrite(department_ids, sizeof(int), record_count, file);
    fclose(file);
}

void load_users_database() {
    FILE* file = fopen(USERS_DB_FILE, "rb");
    if (file == NULL) {
        return;
    }
    fread(&record_count, sizeof(int), 1, file);
    if (record_count > MAX_RECORDS) {
        record_count = MAX_RECORDS;
    }
    fread(ids, sizeof(int), record_count, file);
    fread(names, sizeof(char[50]), record_count, file);
    fread(emails, sizeof(char[50]), record_count, file);
    fread(ages, sizeof(int), record_count, file);

    size_t dep_read = fread(department_ids, sizeof(int), record_count, file);
    if (dep_read != (size_t)record_count) {
        for (int i = 0; i < record_count; i++) {
            department_ids[i] = -1;
        }
    }
    fclose(file);
}

void save_departments_database() {
    FILE* file = fopen(DEPARTMENTS_DB_FILE, "wb");
    if (file == NULL) {
        return;
    }
    fwrite(&dept_count, sizeof(int), 1, file);
    fwrite(dept_ids, sizeof(int), dept_count, file);
    fwrite(dept_names, sizeof(char[50]), dept_count, file);
    fclose(file);
}

void load_departments_database() {
    FILE* file = fopen(DEPARTMENTS_DB_FILE, "rb");
    if (file == NULL) {
        return;
    }

    fread(&dept_count, sizeof(int), 1, file);
    if (dept_count > MAX_DEPARTMENTS) {
        dept_count = MAX_DEPARTMENTS;
    }

    fread(dept_ids, sizeof(int), dept_count, file);
    fread(dept_names, sizeof(char[50]), dept_count, file);
    fclose(file);
}

void upsert_department(int id, const char* name) {
    EnterCriticalSection(&db_critical_section);
    int idx = find_department_index_by_id(id);
    if (idx >= 0) {
        strncpy(dept_names[idx], name, sizeof(dept_names[idx]) - 1);
        dept_names[idx][sizeof(dept_names[idx]) - 1] = '\0';
    } else if (dept_count < MAX_DEPARTMENTS) {
        dept_ids[dept_count] = id;
        strncpy(dept_names[dept_count], name, sizeof(dept_names[dept_count]) - 1);
        dept_names[dept_count][sizeof(dept_names[dept_count]) - 1] = '\0';
        dept_count++;
    }
    save_departments_database();
    LeaveCriticalSection(&db_critical_section);
}

void delete_department(int id) {
    int idx = find_department_index_by_id(id);
    if (idx < 0) return;

    for (int i = idx; i < dept_count - 1; i++) {
        dept_ids[i] = dept_ids[i + 1];
        strcpy(dept_names[i], dept_names[i + 1]);
    }
    dept_count--;

    for (int i = 0; i < record_count; i++) {
        if (department_ids[i] == id) {
            department_ids[i] = -1;
        }
    }

    save_departments_database();
    save_users_database();
}

int assign_department(int user_id, int department_id) {
    EnterCriticalSection(&db_critical_section);
    int uidx = find_user_index_by_id(user_id);
    int didx = find_department_index_by_id(department_id);
    if (uidx < 0 || didx < 0) {
        LeaveCriticalSection(&db_critical_section);
        return 0;
    }
    department_ids[uidx] = department_id;
    save_users_database();
    LeaveCriticalSection(&db_critical_section);
    return 1;
}

void insert_record(int id, const char* name, const char* email, int age, int department_id) {
    if (record_count < MAX_RECORDS) {
        ids[record_count] = id;
        strncpy(names[record_count], name, sizeof(names[record_count]) - 1);
        names[record_count][sizeof(names[record_count]) - 1] = '\0';
        strncpy(emails[record_count], email, sizeof(emails[record_count]) - 1);
        emails[record_count][sizeof(emails[record_count]) - 1] = '\0';
        ages[record_count] = age;
        department_ids[record_count] = department_id;
        record_count++;
        rebuild_user_index();
        save_users_database();
    }
}

void delete_record(int id) {
    int idx = find_user_index_by_id(id);
    if (idx >= 0) {
        for (int j = idx; j < record_count - 1; j++) {
            ids[j] = ids[j + 1];
            strcpy(names[j], names[j + 1]);
            strcpy(emails[j], emails[j + 1]);
            ages[j] = ages[j + 1];
            department_ids[j] = department_ids[j + 1];
        }
        record_count--;
        rebuild_user_index();
        save_users_database();
    }
}


int main() {
    InitializeCriticalSection(&db_critical_section);
    load_users_database();
    load_departments_database();
    rebuild_user_index();
    cache_invalidate_all();

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    struct addrinfo *result = NULL, hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, PORT, &hints, &result) != 0) {
        printf("getaddrinfo failed\n");
        WSACleanup();
        return 1;
    }

    SOCKET listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (listenSocket == INVALID_SOCKET) {
        printf("socket failed\n");
        freeaddrinfo(result);
        WSACleanup();
        return 1;
    }

    if (bind(listenSocket, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        printf("bind failed\n");
        freeaddrinfo(result);
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    freeaddrinfo(result);

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        printf("listen failed\n");
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    printf("Server listening on port %s\n", PORT);

    while (1) {
        SOCKET clientSocket = accept(listenSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET) {
            printf("accept failed\n");
            closesocket(listenSocket);
            WSACleanup();
            return 1;
        }

        char buffer[4096];
        int recvResult = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (recvResult > 0) {
            buffer[recvResult] = '\0';
            char* response = handle_request(buffer);
            send(clientSocket, response, strlen(response), 0);
        }

        closesocket(clientSocket);
    }

    closesocket(listenSocket);
    WSACleanup();
    btree_free(user_index_root);
    DeleteCriticalSection(&db_critical_section);
    return 0;
}
//comment