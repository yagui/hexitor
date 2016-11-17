#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

#include <ncurses.h>

#define READ_BUFFER_SIZE 16 * 1024

typedef struct
{
    WINDOW* window;
    int left;
    int top;
    int width;
    int height;
} pane;

pane hex_pane;
pane detail_pane;

unsigned char* source = NULL;
int source_len;

int cursor_byte;
int cursor_nibble;

int scroll_start;

FILE* log_file;

void setup_pane(pane* pane)
{
    if (pane->window)
    {
        delwin(pane->window);
    }

    pane->window = newwin(pane->height, pane->width, pane->top, pane->left);
}

char nibble_to_hex(unsigned char nibble)
{
    if (nibble < 10)
    {
        return '0' + nibble;
    }

    return 'a' + nibble - 10;
}

unsigned char hex_to_nibble(char hex)
{
    if (hex >= 'a')
    {
        return hex - 'a' + 10;
    }

    return hex - '0';
}

unsigned char first_nibble(unsigned char byte)
{
    return byte >> 4;
}

unsigned char second_nibble(unsigned char byte)
{
    return byte & 0x0f;
}

void byte_to_hex(unsigned char byte, char* hex)
{
    hex[0] = nibble_to_hex(first_nibble(byte));
    hex[1] = nibble_to_hex(second_nibble(byte));
}

unsigned char nibbles_to_byte(unsigned char n0, unsigned char n1)
{
    return n0 << 4 | n1;
}

int bytes_per_line()
{
    return hex_pane.width / 3;
}

int byte_in_line(int byte_offset)
{
    return byte_offset / bytes_per_line();
}

int byte_in_column(int byte_offset)
{
    return byte_offset % bytes_per_line() * 3;
}

int first_byte_in_line(int line_index)
{
    return line_index * bytes_per_line();
}

int last_byte_in_line(int line_index)
{
    return first_byte_in_line(line_index + 1) - 1;
}

int first_visible_byte()
{
    return first_byte_in_line(scroll_start);
}

int last_visible_line()
{
    return scroll_start + hex_pane.height - 1;
}

int last_visible_byte()
{
    int ret = last_byte_in_line(last_visible_line());

    return ret < source_len ? ret : source_len - 1;
}

void render_details()
{
    WINDOW* w = detail_pane.window;
    wclear(w);

    unsigned char* cursor_start = source + cursor_byte;

    mvwprintw(w, 1, 1, "Offset: %d", cursor_byte);
    mvwprintw(w, 2, 1, "Int8:   %d", *(int8_t*)cursor_start);
    mvwprintw(w, 3, 1, "Uint8:  %d", *(uint8_t*)cursor_start);
    mvwprintw(w, 4, 1, "Int16:  %d", *(int16_t*)cursor_start);
    mvwprintw(w, 5, 1, "Uint16: %d", *(uint16_t*)cursor_start);
    mvwprintw(w, 6, 1, "Int32:  %d", *(int32_t*)cursor_start);
    mvwprintw(w, 7, 1, "Uint32: %d", *(uint32_t*)cursor_start);
    mvwprintw(w, 8, 1, "Int64:  %ld", *(int64_t*)cursor_start);
    mvwprintw(w, 9, 1, "UInt64: %ld", *(uint64_t*)cursor_start);

    box(w, 0, 0);
}

void handle_sizing()
{
    static int last_max_x = -1;
    static int last_max_y = -1;

    int max_x;
    int max_y;

    getmaxyx(stdscr, max_y, max_x);

    if (max_y == last_max_y && max_x == last_max_x)
    {
        return;
    }

    // Terminal resized
    last_max_x = max_x;
    last_max_y = max_y;

    hex_pane.width = max_x - 20;
    hex_pane.height = max_y - 20;

    hex_pane.width = max_x - 20;
    //hex_pane.height = max_y - 20;
    hex_pane.height = 5;
    setup_pane(&hex_pane);

    detail_pane.top = hex_pane.top + hex_pane.height + 3;
    detail_pane.width = max_x - 20;
    detail_pane.height = max_y - hex_pane.height - hex_pane.top - 5;
    setup_pane(&detail_pane);
}

void handle_key_left()
{
    if (cursor_nibble == 1)
    {
        cursor_nibble = 0;
    }
    else
    {
        cursor_nibble = 1;
        cursor_byte--;
    }
}

void handle_key_right()
{
    if (cursor_nibble == 0)
    {
        cursor_nibble = 1;
    }
    else
    {
        cursor_nibble = 0;
        cursor_byte++;
    }
}

void handle_key_up()
{
    int temp = cursor_byte - bytes_per_line();

    if (temp >= 0)
    {
        cursor_byte = temp;
    }
}

void handle_key_down()
{
    int temp = cursor_byte + bytes_per_line();

    if (temp < source_len)
    {
        cursor_byte = temp;
    }
}

void handle_overwrite(int event)
{
    // Convert A-F to lower case
    if (event >= 'A' && event <= 'F')
    {
        event += 'a' - 'A';
    }

    // Only process 0-9 and a-f
    if (!((event >= '0' && event <= '9') || (event >= 'a' && event <= 'f')))
    {
        return;
    }

    unsigned char* byte = &source[cursor_byte];

    unsigned char first = first_nibble(*byte);
    unsigned char second = second_nibble(*byte);

    unsigned char* nibble = cursor_nibble ? &second : &first;
    *nibble = hex_to_nibble(event);

    *byte = nibbles_to_byte(first, second);

    handle_key_right();
}

void handle_event(int event)
{
    switch (event)
    {
        case 'h':
        case 'H':
        case KEY_LEFT:  handle_key_left();       break;

        case 'l':
        case 'L':
        case KEY_RIGHT: handle_key_right();      break;

        case 'k':
        case 'K':
        case KEY_UP:    handle_key_up();         break;

        case 'j':
        case 'J':
        case KEY_DOWN:  handle_key_down();       break;

        default:        handle_overwrite(event); break;
    }
}

void clamp_scrolling()
{
    // Clamp to start of buffer
    if (cursor_byte < 0)
    {
        cursor_byte = 0;
        cursor_nibble = 0;
    }

    // Clamp to end of buffer
    if (cursor_byte >= source_len)
    {
        cursor_byte = source_len - 1;
        cursor_nibble = 1;
    }

    // Scroll up if cursor has left viewport
    while (cursor_byte < first_visible_byte())
    {
        scroll_start--;
    }

    // Scroll down if cursor has left viewport
    while (cursor_byte > last_visible_byte())
    {
        scroll_start++;
    }

    // Clamp scroll start to beginning of buffer
    if (scroll_start < 0)
    {
        scroll_start = 0;
    }
}

void render_hex()
{
    wclear(hex_pane.window);

    char hex[2];

    for (int i = first_visible_byte(); i <= last_visible_byte(); i++)
    {
        byte_to_hex(source[i], hex);

        int out_y = byte_in_line(i) - scroll_start;
        int out_x = byte_in_column(i);

        mvwprintw(hex_pane.window, out_y, out_x, "%c%c ", hex[0], hex[1]);
    }
}

void place_cursor()
{
    int render_cursor_x = hex_pane.left +
        byte_in_column(cursor_byte) + cursor_nibble;
    int render_cursor_y = hex_pane.top +
        byte_in_line(cursor_byte) - scroll_start;

    move(render_cursor_y, render_cursor_x);
}

void flush_output()
{
    wnoutrefresh(hex_pane.window);
    wnoutrefresh(detail_pane.window);

    doupdate();
}

void update(int event)
{
    handle_sizing();
    handle_event(event);
    clamp_scrolling();
    render_hex();
    render_details();
    place_cursor();
    flush_output();
}

void open_file(const char* filename)
{
    // Get filesize
    struct stat st;
    stat(filename, &st);
    source_len = st.st_size;
    int count = st.st_size * sizeof(unsigned char);

    // Allocate or reallocate memory
    if (source == NULL)
    {
        source = malloc(count);
    }
    else
    {
        source = realloc(source, count);
    }

    // Read file into memory
    FILE* file = fopen(filename, "r");

    if (!file)
    {
        printf("Error opening file. File not found / permissions problem?\n");
        exit(2);
    }

    unsigned char* target = source;
    size_t bytes_read;

    while ((bytes_read = fread(target, 1, READ_BUFFER_SIZE, file)) > 0)
    {
        target += bytes_read;
    }

    fclose(file);
}

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        printf("Usage: hexitor <filename>\n");
        return 1;
    }

    open_file(argv[1]);

    log_file = fopen("logfile", "w");

    cursor_byte = 0;
    cursor_nibble = 0;
    scroll_start = 0;

    hex_pane.left = 10;
    hex_pane.top = 3;

    detail_pane.left = 10;

    initscr();
    cbreak();
    keypad(stdscr, TRUE);
    refresh();

    update(-1);

    int event;

    while ((event = getch()) != KEY_F(1))
    {
        update(event);
    }

    fclose(log_file);
}
