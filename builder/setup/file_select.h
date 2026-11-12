#ifndef HOOKSHOT_FILE_SELECT_H
#define HOOKSHOT_FILE_SELECT_H

#ifdef __cplusplus
extern "C" {
#endif

int hookshot_file_select_command(const wchar_t *args);
int hookshot_file_select(HINSTANCE instance, const wchar_t *folder, const wchar_t *args, int show);
int hookshot_file_select_latest(char *tag, int length);

#ifdef __cplusplus
}
#endif

#endif
