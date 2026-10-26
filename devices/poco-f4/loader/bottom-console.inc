/* SPDX-License-Identifier: MIT
 * UEFI text protocol extension; a bounded scrolling pane at bottom left.
 * Keep the final firmware row unused to avoid firmware full-screen scrolling.
 */
struct munch_text_mode { int32_t max_mode, mode, attribute, column, row; uint8_t visible; };
struct munch_text_output {
    void *reset;
    efi_status (EFIAPI *output_string)(efi_text_output *, const char16 *);
    void *test_string;
    efi_status (EFIAPI *query_mode)(efi_text_output *, uint64_t, uint64_t *, uint64_t *);
    void *set_mode, *set_attribute, *clear_screen;
    efi_status (EFIAPI *set_cursor)(efi_text_output *, uint64_t, uint64_t);
    void *enable_cursor;
    struct munch_text_mode *mode;
};
static struct munch_text_output *munch_console;
static char16 munch_lines[20][97];
static unsigned munch_rows, munch_cols, munch_top, munch_column;
static void munch_console_init(void)
{
    struct munch_text_output *out = (void *)ST->con_out;
    uint64_t cols = 0, rows = 0;
    if (!out || !out->mode || !out->query_mode || !out->set_cursor ||
        EFI_ERROR(out->query_mode(ST->con_out, out->mode->mode, &cols, &rows)) ||
        cols < 20 || cols > 1024 || rows < 6 || rows > 1024) return;
    munch_cols = cols > 97 ? 96 : (unsigned)cols - 1;
    munch_rows = rows / 3 > 20 ? 20 : (unsigned)rows / 3;
    munch_top = (unsigned)rows - 1 - munch_rows;
    for (unsigned r = 0; r < munch_rows; r++)
        for (unsigned c = 0; c < munch_cols; c++) munch_lines[r][c] = ' ';
    munch_console = out;
}
static void munch_console_char(char c)
{
    if (c == '\r') { munch_column = 0; return; }
    if (c == '\n' || munch_column == munch_cols) {
        for (unsigned r = 1; r < munch_rows; r++)
            for (unsigned x = 0; x < munch_cols; x++) munch_lines[r-1][x] = munch_lines[r][x];
        for (unsigned x = 0; x < munch_cols; x++) munch_lines[munch_rows-1][x] = ' ';
        munch_column = 0;
        for (unsigned r = 0; r < munch_rows; r++) {
            munch_console->set_cursor(ST->con_out, 0, munch_top + r);
            munch_console->output_string(ST->con_out, munch_lines[r]);
        }
        if (c == '\n') return;
    }
    char16 text[2] = {(char16)(unsigned char)c, 0};
    munch_lines[munch_rows-1][munch_column] = text[0];
    munch_console->set_cursor(ST->con_out, munch_column++, munch_top + munch_rows - 1);
    munch_console->output_string(ST->con_out, text);
}
