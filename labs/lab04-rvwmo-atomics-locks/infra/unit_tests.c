#include <stddef.h>

#include "lab_api.h"

_Static_assert(offsetof(struct lab_spinlock, value) == LAB_SPIN_VALUE,
               "spinlock layout mismatch");
_Static_assert(offsetof(struct lab_ticketlock, next) == LAB_TICKET_NEXT,
               "ticket next layout mismatch");
_Static_assert(offsetof(struct lab_ticketlock, owner) == LAB_TICKET_OWNER,
               "ticket owner layout mismatch");
_Static_assert(offsetof(struct lab_mailbox, data) == LAB_MAILBOX_DATA,
               "mailbox data layout mismatch");
_Static_assert(offsetof(struct lab_mailbox, ready) == LAB_MAILBOX_READY,
               "mailbox ready layout mismatch");

unsigned int lab_run_unit_tests(void)
{
    volatile uint64_t atomic_value = 7;
    volatile uint64_t bits = 0;
    struct lab_spinlock spin = {0};
    struct lab_ticketlock ticket = {0};
    struct lab_mailbox mailbox = {0};
    uint64_t output = 0;
    unsigned int failures = 0;
    int passed;

    passed = lab_atomic_fetch_add(&atomic_value, 5) == 7 && atomic_value == 12;
    failures += lab_report("LR/SC fetch-add old and new values", passed);

    passed = lab_spin_trylock(&spin) == 1 && spin.value == 1 &&
             lab_spin_trylock(&spin) == 0 && spin.value == 1;
    lab_spin_unlock(&spin);
    passed = passed && spin.value == 0;
    lab_spin_lock(&spin);
    passed = passed && spin.value == 1;
    lab_spin_unlock(&spin);
    failures += lab_report("spin trylock/lock/unlock contract", passed);

    lab_ticket_lock(&ticket);
    passed = ticket.next == 1 && ticket.owner == 0;
    lab_ticket_unlock(&ticket);
    passed = passed && ticket.owner == 1;
    failures += lab_report("ticket allocation and owner release", passed);

    passed = lab_atomic_test_and_set_bit(&bits, 9) == 0 &&
             bits == (UINT64_C(1) << 9) &&
             lab_atomic_test_and_set_bit(&bits, 9) == 1;
    failures += lab_report("atomic test-and-set bit old value", passed);

    passed = lab_consume_message(&mailbox, &output) == 0;
    lab_publish_message(&mailbox, UINT64_C(LAB_MAILBOX_VALUE));
    passed = passed && lab_consume_message(&mailbox, &output) == 1 &&
             output == UINT64_C(LAB_MAILBOX_VALUE);
    failures += lab_report("release/acquire mailbox publication", passed);
    return failures;
}
