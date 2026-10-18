#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
#include <zlib.h>

#include "snowzip.h"

#define MAX_FILES 4096

#define ID_ADD     101
#define ID_CREATE  102
#define ID_OPEN    103
#define ID_EXTRACT 104
#define ID_REMOVE  105

typedef struct {
    wchar_t *path;
    wchar_t *name;
    uint64_t size;
} InputFile;

typedef struct {
    wchar_t *name;
    uint64_t original_size;
    uint64_t packed_size;
    uint64_t data_offset;
    uint32_t crc;
    uint8_t method;
} ArchiveEntry;

static HINSTANCE g_instance;
static HWND g_window, g_list, g_status;
static InputFile g_inputs[MAX_FILES];
static int g_input_count;
static ArchiveEntry *g_entries;
static uint32_t g_entry_count;
static wchar_t g_archive_path[32768];

static void status(const wchar_t *s)
{
    SetWindowTextW(g_status, s);
}

static void error_box(const wchar_t *s)
{
    MessageBoxW(g_window, s, L"Snow ZIP", MB_OK | MB_ICONERROR);
}

static wchar_t *wdup(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *p = malloc(n * sizeof(*p));
    if (p) memcpy(p, s, n * sizeof(*p));
    return p;
}

static char *to_utf8(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (!n) return NULL;

    char *p = malloc((size_t)n);
    if (!p) return NULL;

    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, p, n, NULL, NULL)) {
        free(p);
        return NULL;
    }
    return p;
}

static wchar_t *from_utf8(const char *s, size_t len)
{
    if (len > INT_MAX) return NULL;

    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                s, (int)len, NULL, 0);
    if (!n) return NULL;

    wchar_t *p = calloc((size_t)n + 1, sizeof(*p));
    if (!p) return NULL;

    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                             s, (int)len, p, n)) {
        free(p);
        return NULL;
    }
    p[n] = L'\0';
    return p;
}

static void clear_inputs(void)
{
    for (int i = 0; i < g_input_count; ++i) {
        free(g_inputs[i].path);
        free(g_inputs[i].name);
    }
    g_input_count = 0;
}

static void clear_archive(void)
{
    for (uint32_t i = 0; i < g_entry_count; ++i)
        free(g_entries[i].name);

    free(g_entries);
    g_entries = NULL;
    g_entry_count = 0;
    g_archive_path[0] = L'\0';
}

static int write_u16(FILE *f, uint16_t v)
{
    return fwrite(&v, sizeof(v), 1, f) == 1;
}

static int write_u32(FILE *f, uint32_t v)
{
    return fwrite(&v, sizeof(v), 1, f) == 1;
}

static int write_u64(FILE *f, uint64_t v)
{
    return fwrite(&v, sizeof(v), 1, f) == 1;
}

static int read_u16(FILE *f, uint16_t *v)
{
    return fread(v, sizeof(*v), 1, f) == 1;
}

static int read_u32(FILE *f, uint32_t *v)
{
    return fread(v, sizeof(*v), 1, f) == 1;
}

static int read_u64(FILE *f, uint64_t *v)
{
    return fread(v, sizeof(*v), 1, f) == 1;
}

static void refresh_list(void)
{
    ListView_DeleteAllItems(g_list);

    for (uint32_t i = 0; i < g_entry_count; ++i) {
        wchar_t size[64], packed[64];

        swprintf(size, 64, L"%llu",
                 (unsigned long long)g_entries[i].original_size);
        swprintf(packed, 64, L"%llu",
                 (unsigned long long)g_entries[i].packed_size);

        LVITEMW item = {0};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.pszText = g_entries[i].name;

        int row = ListView_InsertItem(g_list, &item);
        ListView_SetItemText(g_list, row, 1, size);
        ListView_SetItemText(g_list, row, 2, packed);
    }

    for (int i = 0; i < g_input_count; ++i) {
        wchar_t size[64];

        swprintf(size, 64, L"%llu",
                 (unsigned long long)g_inputs[i].size);

        LVITEMW item = {0};
        item.mask = LVIF_TEXT;
        item.iItem = (int)g_entry_count + i;
        item.pszText = g_inputs[i].name;

        int row = ListView_InsertItem(g_list, &item);
        ListView_SetItemText(g_list, row, 1, size);
        ListView_SetItemText(g_list, row, 2, size);
    }
}

static void add_input(const wchar_t *path)
{
    if (g_input_count >= MAX_FILES) {
        error_box(L"Достигнут предел количества файлов.");
        return;
    }

    DWORD attr = GetFileAttributesW(path);

    if (attr == INVALID_FILE_ATTRIBUTES ||
        (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        error_box(L"Выбери существующий файл, а не папку.");
        return;
    }

    for (int i = 0; i < g_input_count; ++i)
        if (_wcsicmp(g_inputs[i].path, path) == 0)
            return;

    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info))
        return;

    const wchar_t *base = wcsrchr(path, L'\\');
    if (!base) base = path;
    else ++base;

    wchar_t *path_copy = wdup(path);
    wchar_t *name_copy = wdup(base);

    if (!path_copy || !name_copy) {
        free(path_copy);
        free(name_copy);
        error_box(L"Недостаточно памяти.");
        return;
    }

    g_inputs[g_input_count].path = path_copy;
    g_inputs[g_input_count].name = name_copy;
    g_inputs[g_input_count].size =
        ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;

    ++g_input_count;
    refresh_list();
    status(L"Файл добавлен");
}

static void add_dialog(void)
{
    wchar_t paths[32768] = {0};

    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_window;
    ofn.lpstrFilter = L"Все файлы\0*.*\0";
    ofn.lpstrFile = paths;
    ofn.nMaxFile = (DWORD)(sizeof(paths) / sizeof(paths[0]));
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT |
                OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&ofn)) return;

    wchar_t *p = paths + wcslen(paths) + 1;

    if (!*p) {
        add_input(paths);
        return;
    }

    wchar_t dir[32768];
    wcscpy_s(dir, 32768, paths);

    while (*p) {
        wchar_t full[32768];
        swprintf(full, 32768, L"%ls\\%ls", dir, p);
        add_input(full);
        p += wcslen(p) + 1;
    }
}

static int create_archive(const wchar_t *path)
{
    FILE *out = _wfopen(path, L"wb");
    if (!out) {
        error_box(L"Не удалось создать архив.");
        return 0;
    }

    int ok = fwrite(SNZ_MAGIC, 1, SNZ_MAGIC_SIZE, out) == SNZ_MAGIC_SIZE &&
             write_u32(out, (uint32_t)g_input_count);

    for (int i = 0; ok && i < g_input_count; ++i) {
        FILE *in = _wfopen(g_inputs[i].path, L"rb");
        if (!in) {
            ok = 0;
            break;
        }

        if (_fseeki64(in, 0, SEEK_END) != 0) {
            fclose(in);
            ok = 0;
            break;
        }

        long long length = _ftelli64(in);
        if (length < 0 || (uint64_t)length > SIZE_MAX ||
            (uint64_t)length > ULONG_MAX) {
            fclose(in);
            ok = 0;
            break;
        }

        rewind(in);

        size_t n = (size_t)length;
        Bytef *input = malloc(n ? n : 1);
        if (!input) {
            fclose(in);
            ok = 0;
            break;
        }

        if (n && fread(input, 1, n, in) != n) {
            free(input);
            fclose(in);
            ok = 0;
            break;
        }
        fclose(in);

        uLongf capacity = compressBound((uLong)n);
        Bytef *compressed = malloc(capacity ? (size_t)capacity : 1);

        if (!compressed) {
            free(input);
            ok = 0;
            break;
        }

        int zr = compress2(compressed, &capacity, input,
                           (uLong)n, Z_DEFAULT_COMPRESSION);

        uint8_t method = SNZ_METHOD_DEFLATE;
        const Bytef *payload = compressed;
        uint64_t packed_size = (uint64_t)capacity;

        if (zr != Z_OK || capacity >= n) {
            method = SNZ_METHOD_STORE;
            payload = input;
            packed_size = n;
        }

        char *name = to_utf8(g_inputs[i].name);
        if (!name) {
            free(compressed);
            free(input);
            ok = 0;
            break;
        }

        size_t name_len = strlen(name);
        uint32_t crc = snowzip_crc32(input, n);
        uint8_t reserved = 0;

        if (name_len == 0 || name_len > UINT16_MAX) {
            free(name);
            free(compressed);
            free(input);
            ok = 0;
            break;
        }

        ok = write_u16(out, (uint16_t)name_len) &&
             fwrite(&method, 1, 1, out) == 1 &&
             fwrite(&reserved, 1, 1, out) == 1 &&
             write_u64(out, (uint64_t)n) &&
             write_u64(out, packed_size) &&
             write_u32(out, crc) &&
             fwrite(name, 1, name_len, out) == name_len &&
             fwrite(payload, 1, (size_t)packed_size, out) == packed_size;

        free(name);
        free(compressed);
        free(input);

        wchar_t msg[128];
        swprintf(msg, 128, L"Упаковка: %d / %d",
                 i + 1, g_input_count);
        status(msg);
    }

    if (fclose(out) != 0) ok = 0;

    if (!ok) {
        DeleteFileW(path);
        error_box(L"Ошибка создания архива.");
        return 0;
    }

    status(L"Архив успешно создан");
    MessageBoxW(g_window, L"Архив создан!", L"Snow ZIP",
                MB_OK | MB_ICONINFORMATION);
    return 1;
}

static void create_dialog(void)
{
    if (!g_input_count) {
        error_box(L"Сначала добавь файлы.");
        return;
    }

    wchar_t path[32768] = L"archive.snzip";
    OPENFILENAMEW ofn = {0};

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_window;
    ofn.lpstrFilter = L"Snow ZIP (*.snzip)\0*.snzip\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = 32768;
    ofn.lpstrDefExt = L"snzip";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (GetSaveFileNameW(&ofn))
        create_archive(path);
}

static int open_archive(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) {
        error_box(L"Не удалось открыть архив.");
        return 0;
    }

    char magic[SNZ_MAGIC_SIZE];
    uint32_t count = 0;

    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
        memcmp(magic, SNZ_MAGIC, sizeof(magic)) ||
        !read_u32(f, &count) || count > MAX_FILES) {
        fclose(f);
        error_box(L"Неподдерживаемый или повреждённый архив.");
        return 0;
    }

    ArchiveEntry *entries = calloc(count ? count : 1, sizeof(*entries));
    if (!entries) {
        fclose(f);
        error_box(L"Недостаточно памяти.");
        return 0;
    }

    int ok = 1;

    for (uint32_t i = 0; i < count && ok; ++i) {
        uint16_t name_len = 0;
        uint8_t reserved = 0;

        ok = read_u16(f, &name_len) &&
             fread(&entries[i].method, 1, 1, f) == 1 &&
             fread(&reserved, 1, 1, f) == 1 &&
             read_u64(f, &entries[i].original_size) &&
             read_u64(f, &entries[i].packed_size) &&
             read_u32(f, &entries[i].crc);

        if (!ok || !name_len ||
            (entries[i].method != SNZ_METHOD_STORE &&
             entries[i].method != SNZ_METHOD_DEFLATE)) {
            ok = 0;
            break;
        }

        char *name = malloc((size_t)name_len + 1);
        if (!name) {
            ok = 0;
            break;
        }

        if (fread(name, 1, name_len, f) != name_len) {
            free(name);
            ok = 0;
            break;
        }

        name[name_len] = '\0';
        entries[i].name = from_utf8(name, name_len);
        free(name);

        if (!entries[i].name) {
            ok = 0;
            break;
        }

        entries[i].data_offset = (uint64_t)_ftelli64(f);

        if (entries[i].packed_size > LLONG_MAX ||
            _fseeki64(f, (long long)entries[i].packed_size, SEEK_CUR)) {
            ok = 0;
            break;
        }
    }

    fclose(f);

    if (!ok) {
        for (uint32_t i = 0; i < count; ++i)
            free(entries[i].name);
        free(entries);
        error_box(L"Архив повреждён.");
        return 0;
    }

    clear_archive();
    clear_inputs();

    g_entries = entries;
    g_entry_count = count;
    wcsncpy_s(g_archive_path, 32768, path, _TRUNCATE);

    refresh_list();
    status(L"Архив открыт");
    return 1;
}

static void open_dialog(void)
{
    wchar_t path[32768] = L"";
    OPENFILENAMEW ofn = {0};

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_window;
    ofn.lpstrFilter = L"Snow ZIP (*.snzip)\0*.snzip\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = 32768;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&ofn))
        open_archive(path);
}

static int safe_name(const wchar_t *name)
{
    return name && *name &&
           !wcschr(name, L'\\') &&
           !wcschr(name, L'/') &&
           !wcschr(name, L':') &&
           wcscmp(name, L".") &&
           wcscmp(name, L"..");
}

static int extract_entry(FILE *archive, const ArchiveEntry *e,
                         const wchar_t *directory)
{
    if (!safe_name(e->name) ||
        e->original_size > SIZE_MAX ||
        e->packed_size > SIZE_MAX ||
        e->original_size > ULONG_MAX ||
        e->packed_size > ULONG_MAX) {
        error_box(L"Небезопасное имя или слишком большой файл.");
        return 0;
    }

    size_t packed_n = (size_t)e->packed_size;
    size_t output_n = (size_t)e->original_size;

    Bytef *packed = malloc(packed_n ? packed_n : 1);
    Bytef *output = malloc(output_n ? output_n : 1);

    if (!packed || !output) {
        free(packed);
        free(output);
        error_box(L"Недостаточно памяти.");
        return 0;
    }

    int ok = _fseeki64(archive, (long long)e->data_offset, SEEK_SET) == 0 &&
             (!packed_n || fread(packed, 1, packed_n, archive) == packed_n);

    if (ok && e->method == SNZ_METHOD_STORE) {
        ok = packed_n == output_n;
        if (ok && output_n) memcpy(output, packed, output_n);
    } else if (ok) {
        uLongf out_len = (uLongf)output_n;
        ok = uncompress(output, &out_len, packed, (uLong)packed_n) == Z_OK &&
             out_len == output_n;
    }

    if (ok)
        ok = snowzip_crc32(output, output_n) == e->crc;

    if (!ok) {
        free(packed);
        free(output);
        error_box(L"Ошибка распаковки или проверки CRC-32.");
        return 0;
    }

    wchar_t destination[32768];
    swprintf(destination, 32768, L"%ls\\%ls", directory, e->name);

    FILE *out = _wfopen(destination, L"wb");
    if (!out) {
        free(packed);
        free(output);
        error_box(L"Не удалось создать выходной файл.");
        return 0;
    }

    ok = !output_n || fwrite(output, 1, output_n, out) == output_n;
    if (fclose(out) != 0) ok = 0;

    free(packed);
    free(output);

    if (!ok) {
        DeleteFileW(destination);
        error_box(L"Не удалось записать файл.");
    }

    return ok;
}

static void extract_dialog(void)
{
    if (!g_archive_path[0]) {
        error_box(L"Сначала открой архив.");
        return;
    }

    BROWSEINFOW bi = {0};
    bi.hwndOwner = g_window;
    bi.lpszTitle = L"Выбери папку распаковки";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;

    wchar_t directory[MAX_PATH] = L"";
    BOOL ok = SHGetPathFromIDListW(pidl, directory);
    CoTaskMemFree(pidl);

    if (!ok) {
        error_box(L"Не удалось получить путь к папке.");
        return;
    }

    FILE *f = _wfopen(g_archive_path, L"rb");
    if (!f) {
        error_box(L"Не удалось открыть архив.");
        return;
    }

    for (uint32_t i = 0; i < g_entry_count; ++i) {
        wchar_t msg[128];
        swprintf(msg, 128, L"Распаковка: %u / %u",
                 i + 1, g_entry_count);
        status(msg);

        if (!extract_entry(f, &g_entries[i], directory)) {
            fclose(f);
            return;
        }
    }

    fclose(f);
    status(L"Распаковка завершена");
    MessageBoxW(g_window, L"Готово!", L"Snow ZIP",
                MB_OK | MB_ICONINFORMATION);
}

static void remove_selected(void)
{
    int row = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (row < 0) return;

    if (g_archive_path[0]) {
        error_box(L"Удаление из существующего архива пока не поддерживается.");
        return;
    }

    if (row >= g_input_count) return;

    free(g_inputs[row].path);
    free(g_inputs[row].name);

    for (int i = row; i < g_input_count - 1; ++i)
        g_inputs[i] = g_inputs[i + 1];

    --g_input_count;
    refresh_list();
    status(L"Файл удалён");
}

static HWND make_button(HWND parent, const wchar_t *label, int id,
                        int x, int width)
{
    return CreateWindowExW(
        0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, 12, width, 34, parent, (HMENU)(INT_PTR)id,
        g_instance, NULL
    );
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg,
                                    WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        make_button(hwnd, L"+ Добавить", ID_ADD, 12, 112);
        make_button(hwnd, L"Создать", ID_CREATE, 132, 100);
        make_button(hwnd, L"Открыть", ID_OPEN, 240, 100);
        make_button(hwnd, L"Распаковать", ID_EXTRACT, 348, 120);
        make_button(hwnd, L"Удалить", ID_REMOVE, 476, 100);

        g_list = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS,
            12, 60, 760, 390, hwnd, NULL, g_instance, NULL
        );

        ListView_SetExtendedListViewStyle(
            g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                    LVS_EX_GRIDLINES
        );

        LVCOLUMNW col = {0};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = L"Имя файла";
        col.cx = 390;
        ListView_InsertColumn(g_list, 0, &col);
        col.pszText = L"Размер";
        col.cx = 165;
        ListView_InsertColumn(g_list, 1, &col);
        col.pszText = L"В архиве";
        col.cx = 165;
        ListView_InsertColumn(g_list, 2, &col);

        g_status = CreateWindowExW(
            0, L"STATIC", L"Готов к работе",
            WS_CHILD | WS_VISIBLE,
            12, 462, 760, 24, hwnd, NULL, g_instance, NULL
        );

        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);

        for (UINT i = 0; i < count; ++i) {
            wchar_t path[32768];

            if (DragQueryFileW(drop, i, path, 32768))
                add_input(path);
        }

        DragFinish(drop);
        return 0;
    }

    case WM_SIZE: {
        int width = LOWORD(lp);
        int height = HIWORD(lp);

        if (g_list)
            MoveWindow(g_list, 12, 60, width - 24,
                       height - 100, TRUE);

        if (g_status)
            MoveWindow(g_status, 12, height - 30,
                       width - 24, 20, TRUE);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ADD: add_dialog(); return 0;
        case ID_CREATE: create_dialog(); return 0;
        case ID_OPEN: open_dialog(); return 0;
        case ID_EXTRACT: extract_dialog(); return 0;
        case ID_REMOVE: remove_selected(); return 0;
        }
        break;

    case WM_DESTROY:
        clear_inputs();
        clear_archive();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous,
                    PWSTR command_line, int show_command)
{
    (void)previous;
    (void)command_line;

    g_instance = instance;

    INITCOMMONCONTROLSEX cc = {0};
    cc.dwSize = sizeof(cc);
    cc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&cc);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = window_proc;
    wc.lpszClassName = L"SnowZIPWindow";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(GetModuleHandleW(NULL),
             MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&wc)) {
        CoUninitialize();
        return 1;
    }

    g_window = CreateWindowExW(
        0, wc.lpszClassName, L"Snow ZIP",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 540,
        NULL, NULL, instance, NULL
    );

    if (!g_window) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(g_window, show_command);
    UpdateWindow(g_window);

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv) {
        if (argc >= 3 && wcscmp(argv[1], L"--open") == 0) {
            open_archive(argv[2]);
        } else if (argc >= 3 && wcscmp(argv[1], L"--add") == 0) {
            add_input(argv[2]);
        } else if (argc >= 2) {
            const wchar_t *ext = wcsrchr(argv[1], L'.');

            if (ext && _wcsicmp(ext, L".snzip") == 0)
                open_archive(argv[1]);
            else
                add_input(argv[1]);
        }

        LocalFree(argv);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
