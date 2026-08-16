SRC=../app/src
CFLAGS=-Wall -Wextra -I$(SRC)
TESTS=test_buttons test_wiced_hci

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done

test_buttons: test_buttons.c $(SRC)/buttons.c $(SRC)/buttons.h
	$(CC) $(CFLAGS) -DBUTTONS_HOST_TEST -o $@ test_buttons.c $(SRC)/buttons.c

test_wiced_hci: test_wiced_hci.c $(SRC)/wiced_hci.c $(SRC)/wiced_hci.h
	$(CC) $(CFLAGS) -o $@ test_wiced_hci.c $(SRC)/wiced_hci.c

clean:
	rm -f $(TESTS)

.PHONY: test clean
