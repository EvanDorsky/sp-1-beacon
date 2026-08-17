SRC=../app/src
CFLAGS=-Wall -Wextra -I$(SRC)
TESTS=test_buttons test_wiced_hci test_beacon_state test_cybt_dl

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done

test_buttons: test_buttons.c $(SRC)/buttons.c $(SRC)/buttons.h
	$(CC) $(CFLAGS) -DBUTTONS_HOST_TEST -o $@ test_buttons.c $(SRC)/buttons.c

test_wiced_hci: test_wiced_hci.c $(SRC)/wiced_hci.c $(SRC)/wiced_hci.h
	$(CC) $(CFLAGS) -o $@ test_wiced_hci.c $(SRC)/wiced_hci.c

test_beacon_state: test_beacon_state.c $(SRC)/beacon_state.c $(SRC)/beacon_state.h
	$(CC) $(CFLAGS) -o $@ test_beacon_state.c $(SRC)/beacon_state.c

test_cybt_dl: test_cybt_dl.c $(SRC)/cybt_dl.c $(SRC)/cybt_dl.h
	$(CC) $(CFLAGS) -o $@ test_cybt_dl.c $(SRC)/cybt_dl.c

clean:
	rm -f $(TESTS)

.PHONY: test clean
