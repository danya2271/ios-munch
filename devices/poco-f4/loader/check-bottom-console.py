#!/usr/bin/env python3
"""Check pane bounds and wrapping with a mocked UEFI console."""
from pathlib import Path
import subprocess, tempfile
HEADER = '#include <stdint.h>\n#include <assert.h>\n#include <string.h>\ntypedef uint16_t char16;\ntypedef uint64_t efi_status;\n#define EFIAPI\n#define EFI_ERROR(x) ((x)>>63)\ntypedef struct efi_text_output {void *reset; efi_status (*output_string)(struct efi_text_output *, const char16 *);} efi_text_output;\nstatic struct {efi_text_output *con_out;} table, *ST=&table;\n'
TEST = "\nstatic unsigned x,y,writes;\nstatic efi_status query(efi_text_output *o,uint64_t m,uint64_t *c,uint64_t *r) {(void)o;(void)m;*c=135;*r=150;return 0;}\nstatic efi_status cursor(efi_text_output *o,uint64_t c,uint64_t r) {(void)o; assert(c<96 && r>=129 && r<149);x=c;y=r;return 0;}\nstatic efi_status output(efi_text_output *o,const char16 *s) {(void)o;while(*s++) {assert(x++<135);writes++;}return 0;}\nint main(void) {\n struct munch_text_mode mode={0}; struct munch_text_output out={0};\n out.query_mode=query;out.set_cursor=cursor;out.output_string=output;out.mode=&mode;\n ST->con_out=(void*)&out;munch_console_init();assert(munch_console && munch_top==129);\n for(unsigned i=0;i<100;i++) {for(unsigned j=0;j<110;j++)munch_console_char('X');munch_console_char(10);}\n assert(writes>10000 && y<149);\n ST->con_out=0;munch_console=0;munch_console_init();assert(!munch_console);\n return 0;\n}\n"
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    (p/'test.c').write_text(HEADER + Path(__file__).with_name('bottom-console.inc').read_text() + TEST)
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS: bottom pane bounds, long-line wrapping, scrolling, missing console')
