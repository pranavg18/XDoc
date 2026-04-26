#include "auth.h"

Role authenticate(const char *username, const char *password) {
    FILE *fp = fopen(USERS_FILE, "r");
    if (!fp) {
        // deny everyone if the users file is missing
        fprintf(stderr, "[Auth] Cannot open '%s'. Denying login.\n", USERS_FILE);
        return DENIED;
    }

    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0'; // remove trailing newline

        // parse username:password:role
        char user[32], pwd[32];
        int role;

        if (sscanf(line, "%31[^:]:%31[^:]:%d", user, pwd, &role) != 3)
            continue;
        if (strcmp(username, user) == 0) {
            fclose(fp);
            if (strcmp(password, pwd) == 0)
                return (Role)role;
            else
                return DENIED;
        }
    }

    fclose(fp);
    return DENIED;  // username not found
}
