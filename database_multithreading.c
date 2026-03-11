#define _WIN32_WINNT 0x0600
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#define MAX_RECORDS 100
#define DB_FILE "users.db"
int ids[MAX_RECORDS];
char names[MAX_RECORDS][50];
char emails[MAX_RECORDS][50];
int ages[MAX_RECORDS];
int record_count = 0;

CRITICAL_SECTION db_critical_section;


const char* sample_names[] = {
    "allo Johnson", "Bob moNgol", "sami Brown", "aNN ASS", "AIOOOSSS Adam",
    "SALAH hamza", " iwmdi Lee", "Henry fewf", "Ifwey f", "fewf wefw"
};
const char* sample_emails[] = {
    "alice@example.com", "bob@example.com", "charlie@example.com", "diana@example.com", "eve@example.com",
    "frank@example.com", "grace@example.com", "henry@example.com", "ivy@example.com", "jack@example.com"
};
const int sample_ages[] = {25, 30, 35, 28, 22, 40, 33, 27, 29, 31};

#define PORT "8080"

typedef struct {
    char key[50];
    char value[100];
} Param;

int parse_query(char* query, Param* params, int max_params) {
    int count = 0;
    char* token = strtok(query, "&");
    while (token && count < max_params) {
        char* eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            strcpy(params[count].key, token);
            strcpy(params[count].value, eq + 1);
            count++;
        }
        token = strtok(NULL, "&");
    }
    return count;
}

char* handle_request(char* request) {
    static char response[4096];
    char method[10], path[100], version[10];
    sscanf(request, "%s %s %s", method, path, version);
    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/select") == 0) {
            strcpy(response, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n[");
            for (int i = 0; i < record_count; i++) {
                char buf[256];
                sprintf(buf, "{\"id\":%d,\"name\":\"%s\",\"email\":\"%s\",\"age\":%d}", ids[i], names[i], emails[i], ages[i]);
                strcat(response, buf);
                if (i < record_count - 1) strcat(response, ",");
            }
            strcat(response, "]");
        } else if (strncmp(path, "/select/", 8) == 0) {
            int id = atoi(path + 8);
            int found = 0;
            for (int i = 0; i < record_count; i++) {
                if (ids[i] == id) {
                    sprintf(response, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"id\":%d,\"name\":\"%s\",\"email\":\"%s\",\"age\":%d}", ids[i], names[i], emails[i], ages[i]);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                strcpy(response, "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"error\":\"Not found\"}");
            }
        } else {
            strcpy(response, "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/insert") == 0) {
        char* cl = strstr(request, "Content-Length:");
        if (cl) {
            int len = atoi(cl + 15);
            char* body = strstr(request, "\r\n\r\n") + 4;
            Param params[10];
            int n = parse_query(body, params, 10);
            int id = -1, age = -1;
            char name[50] = "", email[50] = "";
            for (int i = 0; i < n; i++) {
                if (strcmp(params[i].key, "id") == 0) id = atoi(params[i].value);
                else if (strcmp(params[i].key, "name") == 0) strcpy(name, params[i].value);
                else if (strcmp(params[i].key, "email") == 0) strcpy(email, params[i].value);
                else if (strcmp(params[i].key, "age") == 0) age = atoi(params[i].value);
            }
            if (id != -1 && name[0] && email[0] && age != -1) {
                int exists = 0;
                for (int i = 0; i < record_count; i++) if (ids[i] == id) exists = 1;
                if (!exists && record_count < MAX_RECORDS) {
                    insert_record(id, name, email, age);
                    save_database();
                    strcpy(response, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":true,\"message\":\"Inserted\"}");
                } else {
                    strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"message\":\"ID exists or full\"}");
                }
            } else {
                strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
            }
        } else {
            strcpy(response, "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        }
    } else if (strcmp(method, "DELETE") == 0 && strncmp(path, "/delete/", 8) == 0) {
        int id = atoi(path + 8);
        int found = 0;
        for (int i = 0; i < record_count; i++) {
            if (ids[i] == id) {
                delete_record(id);
                save_database();
                found = 1;
                break;
            }
        }
        if (found) {
            strcpy(response, "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":true,\"message\":\"Deleted\"}");
        } else {
            strcpy(response, "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"message\":\"Not found\"}");
        }
    } else {
        strcpy(response, "HTTP/1.1 405 Method Not Allowed\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
    }
    return response;
}


void save_database() {
    FILE* file = fopen(DB_FILE, "wb");
    if (file == NULL) {
        return;
    }
    fwrite(&record_count, sizeof(int), 1, file);
    fwrite(ids, sizeof(int), record_count, file);
    fwrite(names, sizeof(char[50]), record_count, file);
    fwrite(emails, sizeof(char[50]), record_count, file);
    fwrite(ages, sizeof(int), record_count, file);
    fclose(file);
}


void load_database() {
    FILE* file = fopen(DB_FILE, "rb");
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
    fclose(file);
}


void insert_record(int id, const char* name, const char* email, int age) {
    EnterCriticalSection(&db_critical_section);

    for (int i = 0; i < record_count; i++) {
        if (ids[i] == id) {
            LeaveCriticalSection(&db_critical_section);
            return;
        }
    }
    if (record_count < MAX_RECORDS) {
        ids[record_count] = id;
        strcpy(names[record_count], name);
        strcpy(emails[record_count], email);
        ages[record_count] = age;
        record_count++;
    }
    save_database();
    LeaveCriticalSection(&db_critical_section);
}

void delete_record(int id) {
    EnterCriticalSection(&db_critical_section);
    for (int i = 0; i < record_count; i++) {
        if (ids[i] == id) {
            for (int j = i; j < record_count - 1; j++) {
                ids[j] = ids[j + 1];
                strcpy(names[j], names[j + 1]);
                strcpy(emails[j], emails[j + 1]);
                ages[j] = ages[j + 1];
            }
            record_count--;
            LeaveCriticalSection(&db_critical_section);
            return;
        }
    }
    save_database();
    LeaveCriticalSection(&db_critical_section);
}


int main() {
    load_database();
    InitializeCriticalSection(&db_critical_section);

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
    DeleteCriticalSection(&db_critical_section);
    return 0;
}
