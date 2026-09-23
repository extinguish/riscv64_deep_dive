#ifndef LAB_LAYOUT_H
#define LAB_LAYOUT_H

#define LAB_PAGE_SIZE 4096
#define LAB_STACK_SIZE 16384

#define LAB_PMP_R 0x01
#define LAB_PMP_W 0x02
#define LAB_PMP_X 0x04
#define LAB_PMP_A_OFF 0
#define LAB_PMP_A_TOR 1
#define LAB_PMP_A_NA4 2
#define LAB_PMP_A_NAPOT 3
#define LAB_PMP_A_SHIFT 3
#define LAB_PMP_L 0x80

#define LAB_CAUSE_LOAD_ACCESS_FAULT 5
#define LAB_CAUSE_STORE_ACCESS_FAULT 7

#endif
