cat > Makefile << 'EOF'
CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lcurl -lgpiod
TARGET = soil_monitor

all: $(TARGET)

$(TARGET): Main.c
	$(CC) $(CFLAGS) -o $(TARGET) Main.c $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
EOF