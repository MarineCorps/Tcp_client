# 컴파일러 및 옵션 설정
CC = gcc
CFLAGS = -Wall
TARGET = client_stream
SRC = client_stream.c

# 기본 타겟: 클라이언트 빌드
all: $(TARGET) myread.so

# 클라이언트 빌드
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

# read 인터포지션용 공유 라이브러리 빌드
myread.so: myread.c
	$(CC) $(CFLAGS) -DRUNTIME -fPIC -shared -o myread.so myread.c -ldl
libnetprof.so: netprof.c
	$(CC) $(CFLAGS) -fPIC -pthread -shared -o libnetprof.so netprof.c -ldl
# 정리
clean:
	rm -f $(TARGET) *.bin *.so
