// tests/dump_pet_strings.c
// 给 tests/test_pet_font_coverage.py 用的小工具: 把界面文案清单打印出来。
//
// 每行格式: <key>\t<px>\t<utf8 text>
//
// 文案直接从 main/pet_strings.h 的 X 宏清单展开, 所以测试核对的永远是「界面上
// 真正会显示的字符串」, 不会出现测试里另抄一份、两边慢慢跑偏的情况。
#include <stdio.h>

#include "pet_strings.h"

// 分号写在宏里: X 宏展开出来是一串语句, 需要自己带分隔。
#define PET_DUMP_ENTRY(key, text, px) \
    printf("%s\t%d\t%s\n", #key, (int)(px), text);

int main(void)
{
    PET_UI_STRING_LIST(PET_DUMP_ENTRY)
    return 0;
}
