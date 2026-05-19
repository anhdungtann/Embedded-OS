#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <mqueue.h>
#include <linux/input.h>
#include <mosquitto.h>
#include <time.h>
#include "OLED_LCD_SSD1306.h"
#include "fonts.h"
#include <signal.h>
// --- Cấu hình thiết bị ---
#define LED_DEVICE "/dev/led_test"
#define BUTTON_DEVICE "/dev/input/my_button"

// --- Cấu hình MQTT ---
#define MQTT_HOST "broker.emqx.io"
#define MQTT_PORT 1883
#define MQTT_TOPIC_PUB "pzem/data"
#define MQTT_TOPIC_SUB "pzem/config"

// --- Cấu hình Message Queue ---
#define QUEUE_LIMIT      "/limit_queue"
#define MAX_MSG_SIZE    sizeof(float)
#define MAX_MSGS        10
#define QUEUE_PZEM_OLED "/pzem_oled_q"
#define QUEUE_PZEM_MQTT "/pzem_mqtt_q"
#define QUEUE_PZEM_LED "/pzem_led_q"
#define MAX_PZEM_STR    128

// --- Biến toàn cục ---
float current_threshold = 20.0;
const float STEP = 5.0;
const float MAX_THRESHOLD = 500.0;
const float MIN_THRESHOLD = 0.0;
int fd_oled = -1;
SSD1306_Name myOLED;
struct mosquitto *global_mosq = NULL;

mqd_t mq_limit, mq_pzem_oled, mq_pzem_mqtt, mq_pzem_led;
pthread_mutex_t lock_threshold;

void handle_sigint(int sig) {
    printf("\nĐang đóng chương trình và dọn dẹp tài nguyên...\n");

    // 1. Ngắt kết nối MQTT an toàn để broker không bị treo session
    if (global_mosq) {
        mosquitto_disconnect(global_mosq);
        mosquitto_destroy(global_mosq);
    }

    // 2. Tắt màn hình OLED hoặc hiển thị chữ "ERROR" trước khi sập
    SSD1306_Fill(&myOLED, SSD1306_COLOR_BLACK);
    SSD1306_GotoXY(&myOLED, 10, 0);
    SSD1306_Puts(&myOLED, "!! SHUTTING DOWN !!", &Font_7x10, SSD1306_COLOR_WHITE);
    SSD1306_UpdateScreen(&myOLED);
    close(fd_oled);

    // Đóng Message Queue
    mq_close(mq_limit);
    mq_close(mq_pzem_oled);
    mq_close(mq_pzem_mqtt);
    mq_close(mq_pzem_led);
    // Xóa Message Queue khỏi hệ thống
    mq_unlink(QUEUE_LIMIT);
    mq_unlink(QUEUE_PZEM_OLED);
    mq_unlink(QUEUE_PZEM_MQTT);
    mq_unlink(QUEUE_PZEM_LED);

    pthread_mutex_destroy(&lock_threshold);

    printf("Don dep hoan tat. Chu dong thoat de Script Init.d tu khoi dong lai!\n");
    exit(0);
}

// --- Luồng 1: Đọc từ Driver PZEM (Producer) ---
void* thread_read_pzem(void* arg) {
    char buf[MAX_PZEM_STR];
    struct timespec ts;

    while(1) {
        // Bước 1: Mở file
        int fd = open("/dev/pzem_sensor", O_RDONLY);
        if (fd >= 0) {
            memset(buf, 0, sizeof(buf));
            int n = read(fd, buf, sizeof(buf) - 1);
            close(fd); 

            if (n > 0) {
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 100000000; // Đợi tối đa 100ms
                if (ts.tv_nsec >= 1000000000) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000; }

                mq_timedsend(mq_pzem_oled, buf, MAX_PZEM_STR, 0, &ts);
                mq_timedsend(mq_pzem_mqtt, buf, MAX_PZEM_STR, 0, &ts);
                mq_timedsend(mq_pzem_led,  buf, MAX_PZEM_STR, 0, &ts);
            }
        } else {
            perror("PZEM Open Failed");
        }

        usleep(500000); 
    }
    return NULL;
}


// --- Luồng 2: Hiển thị OLED (Consumer trung tâm) ---
void* thread_display_oled(void* arg) {
    char display_buf[MAX_PZEM_STR] = "Loading...";
    char v_raw[16], i_raw[16], p_raw[16], f_raw[16];
    char i_fmt[16], p_fmt[16], limit_fmt[16];
    float received_limit;
    float current_display_limit = 10.0; // Giá trị nội bộ
    struct timespec ts;

    while(1) {
        // 1. Kiểm tra Queue Ngưỡng (Timeout 10ms - check nhanh)
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10000000; 
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000; }

        if (mq_timedreceive(mq_limit, (char*)&received_limit, MAX_MSG_SIZE, NULL, &ts) >= 0) {
            current_display_limit = received_limit;
            printf("Display: Cập nhật ngưỡng hiển thị: %.0fW\n", current_display_limit);
        }

        // 2. Kiểm tra Queue Dữ liệu PZEM (Timeout 40ms)
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 40000000; 
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000; }

        if (mq_timedreceive(mq_pzem_oled, display_buf, MAX_PZEM_STR, NULL, &ts) >= 0) {
            // Chỉ vẽ khi nhận được dữ liệu PZEM mới
            if (sscanf(display_buf, "U: %[^|] | I: %[^|] | P: %[^|] | F: %[^|]", 
                       v_raw, i_raw, p_raw, f_raw) >= 4) {

                snprintf(i_fmt, sizeof(i_fmt), "%.2fA", atof(i_raw));
                snprintf(p_fmt, sizeof(p_fmt), "%.2fW", atof(p_raw));
                snprintf(limit_fmt, sizeof(limit_fmt), "Limit:%.0fW", current_display_limit);

                SSD1306_Fill(&myOLED, SSD1306_COLOR_BLACK);
                
                SSD1306_GotoXY(&myOLED, 10, 0);
                SSD1306_Puts(&myOLED, "POWER MONITOR", &Font_7x10, SSD1306_COLOR_WHITE);

                SSD1306_GotoXY(&myOLED, 0, 18);
                SSD1306_Puts(&myOLED, "V:", &Font_7x10, SSD1306_COLOR_WHITE);
                SSD1306_GotoXY(&myOLED, 15, 18);
                SSD1306_Puts(&myOLED, v_raw, &Font_7x10, SSD1306_COLOR_WHITE);

                SSD1306_GotoXY(&myOLED, 65, 18);
                SSD1306_Puts(&myOLED, "I:", &Font_7x10, SSD1306_COLOR_WHITE);
                SSD1306_GotoXY(&myOLED, 80, 18);
                SSD1306_Puts(&myOLED, i_fmt, &Font_7x10, SSD1306_COLOR_WHITE);

                SSD1306_GotoXY(&myOLED, 0, 33);
                SSD1306_Puts(&myOLED, "P:", &Font_7x10, SSD1306_COLOR_WHITE);
                SSD1306_GotoXY(&myOLED, 15, 33);
                SSD1306_Puts(&myOLED, p_fmt, &Font_7x10, SSD1306_COLOR_WHITE);

                SSD1306_GotoXY(&myOLED, 0, 48);
                SSD1306_Puts(&myOLED, limit_fmt, &Font_7x10, SSD1306_COLOR_WHITE);

                SSD1306_UpdateScreen(&myOLED);
            }
        }
        usleep(100000); 
    }
    return NULL;
}

// --- Luồng 3: Điều khiển LED ---
void* thread_led_blink(void* arg) {
    int fd_led = open(LED_DEVICE, O_WRONLY);
    if (fd_led < 0) return NULL;

    char led_buf[MAX_PZEM_STR];
    char p_raw[16];
    struct timespec ts;

    while(1) {
        // Nhận dữ liệu công suất từ Queue PZEM_LED
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000; // timeout 50ms
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000; }

        if (mq_timedreceive(mq_pzem_led, led_buf, MAX_PZEM_STR, NULL, &ts) >= 0) {
            if (sscanf(led_buf, "U: %*[^|] | I: %*[^|] | P: %[^|] | F: %*[^|]", p_raw) == 1) {
                float p_val = atof(p_raw);
                float threshold_copy;

                // Đọc ngưỡng với mutex
                pthread_mutex_lock(&lock_threshold);
                threshold_copy = current_threshold;
                pthread_mutex_unlock(&lock_threshold);

                if (p_val > threshold_copy) {
                    // Quá tải → LED nhấp nháy
                    write(fd_led, "1", 1); usleep(200000);
                    write(fd_led, "0", 1); usleep(200000);
                } else {
                    // Bình thường → LED tắt
                    write(fd_led, "0", 1); usleep(500000);
                }
            }
        }
    }
    close(fd_led);
    return NULL;
}


// --- Luồng 4: Nút nhấn (Producer) ---
void* thread_button_handler(void* arg) {
    int fd_btn = open("/dev/input/event1", O_RDONLY);
    struct input_event ev;

    if (fd_btn < 0) {
        perror("Lỗi: Không thể mở /dev/input/event1");
        return NULL;
    }

    while(1) {
        if (read(fd_btn, &ev, sizeof(struct input_event)) > 0) {
            // Chỉ xử lý khi có sự kiện nhấn phím (value == 1)
            if (ev.type == EV_KEY && ev.value == 1) {
                
                pthread_mutex_lock(&lock_threshold);
                if (ev.code == KEY_UP) {
                    // Kiểm tra nếu cộng thêm STEP vẫn chưa vượt MAX thì mới cộng
                    if (current_threshold + STEP <= MAX_THRESHOLD) {
                        current_threshold += STEP;
                        printf("[Nút UP] Ngưỡng tăng: %.1fW\n", current_threshold);
                    } else {
                        current_threshold = MAX_THRESHOLD; // Ép về MAX
                        printf("[Nút UP] Đã đạt giới hạn tối đa: %.1fW\n", MAX_THRESHOLD);
                    }
                } 
                else if (ev.code == KEY_DOWN) {
                    // Kiểm tra nếu trừ đi STEP vẫn không bé hơn MIN thì mới trừ
                    if (current_threshold - STEP >= MIN_THRESHOLD) {
                        current_threshold -= STEP;
                        printf("[Nút DOWN] Ngưỡng giảm: %.1fW\n", current_threshold);
                    } else {
                        current_threshold = MIN_THRESHOLD; // Ép về MIN
                        printf("[Nút DOWN] Đã đạt giới hạn tối thiểu: %.1fW\n", MIN_THRESHOLD);
                    }
                }
                float val_to_send = current_threshold;
                pthread_mutex_unlock(&lock_threshold);
                // Gửi giá trị ngưỡng mới vào Message Queue
                if (mq_send(mq_limit, (const char*)&val_to_send, sizeof(val_to_send), 0) == -1) {
                    perror("Lỗi mq_send trong luồng nút nhấn");
                }
            }
        }
    }
    close(fd_btn);
    return NULL;
}

void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
    if (msg->payloadlen > 0) {
        char temp_buf[32];
        int len = (msg->payloadlen < 31) ? msg->payloadlen : 31;
        memcpy(temp_buf, msg->payload, len);
        temp_buf[len] = '\0';
        
        float new_limit = atof(temp_buf);
        if (new_limit >= MIN_THRESHOLD && new_limit <= MAX_THRESHOLD) {

            pthread_mutex_lock(&lock_threshold);
            current_threshold = new_limit;
            pthread_mutex_unlock(&lock_threshold);
            // Gửi vào chung Queue với Button
            mq_send(mq_limit, (const char*)&new_limit, sizeof(new_limit), 0);
            printf("MQTT: Đã đẩy ngưỡng %.1f vào Queue\n", new_limit);
        }
    }
}

void* thread_mqtt(void* arg) {
    char mqtt_buf[MAX_PZEM_STR];
    struct timespec ts;

    mosquitto_lib_init();
    global_mosq = mosquitto_new("BBB_Chien_Task", true, NULL);
    mosquitto_message_callback_set(global_mosq, on_message);
    
    if (mosquitto_connect(global_mosq, MQTT_HOST, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS) return NULL;
    mosquitto_subscribe(global_mosq, NULL, MQTT_TOPIC_SUB, 0);
    
    // Chạy loop trong thread riêng của thư viện để luôn sẵn sàng nhận tin (Subscribe)
    mosquitto_loop_start(global_mosq);

    while(1) {
        // Đợi dữ liệu PZEM dành riêng cho MQTT
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000; // Đợi tối đa 500ms
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }

        if (mq_timedreceive(mq_pzem_mqtt, mqtt_buf, MAX_PZEM_STR, NULL, &ts) >= 0) {
            mosquitto_publish(global_mosq, NULL, MQTT_TOPIC_PUB, strlen(mqtt_buf), mqtt_buf, 0, false);
        }
    }
    return NULL;
}

// --- Hàm Main ---
int main() {
    pthread_t t1, t2, t3, t4, t5;

    // 1. Đăng ký xử lý tín hiệu để dọn dẹp khi bị tắt
    signal(SIGSEGV, handle_sigint); // Lỗi truy cập vùng nhớ trái phép
    signal(SIGABRT, handle_sigint); // Lỗi khi hàm assert thất bại hoặc gọi abort()
    signal(SIGINT, handle_sigint); // Tín hiệu khi bấm Ctrl+C để test
    signal(SIGTERM, handle_sigint); // Tín hiệu khi bị pkill hoặc kill thông thường

    fd_oled = open("/dev/oled_ssd1306", O_WRONLY);
    if (fd_oled < 0) {
        perror("Lỗi mở thiết bị OLED");
        return -1;
    }

    if (SSD1306_Init(&myOLED) != 0) {
        printf("Lỗi khởi tạo OLED!\n");
        close(fd_oled);
        return -1;
    }

    SSD1306_Fill(&myOLED, SSD1306_COLOR_BLACK);
    SSD1306_UpdateScreen(&myOLED);

    struct mq_attr attr_f = {0, 10, sizeof(float), 0};
    struct mq_attr attr_s = {0, 20, MAX_PZEM_STR, 0};

    mq_unlink(QUEUE_LIMIT); mq_unlink(QUEUE_PZEM_OLED); mq_unlink(QUEUE_PZEM_MQTT); mq_unlink(QUEUE_PZEM_LED);
    mq_limit = mq_open(QUEUE_LIMIT, O_CREAT | O_RDWR, 0644, &attr_f);
    mq_pzem_oled = mq_open(QUEUE_PZEM_OLED, O_CREAT | O_RDWR, 0644, &attr_s);
    mq_pzem_mqtt = mq_open(QUEUE_PZEM_MQTT, O_CREAT | O_RDWR, 0644, &attr_s);
    mq_pzem_led = mq_open(QUEUE_PZEM_LED, O_CREAT | O_RDWR, 0644, &attr_s);

    if (pthread_mutex_init(&lock_threshold, NULL) != 0) {
            printf("Lỗi: Không thể khởi tạo Mutex!\n");
            return -1;
    }

    // Tạo 5 luồng
    pthread_create(&t1, NULL, thread_read_pzem, NULL);
    pthread_create(&t2, NULL, thread_display_oled, NULL);
    pthread_create(&t3, NULL, thread_led_blink, NULL);
    pthread_create(&t4, NULL, thread_button_handler, NULL);
    pthread_create(&t5, NULL, thread_mqtt, NULL);

    // Chờ các luồng (Thực tế app chạy vô tận)
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    pthread_join(t4, NULL);
    pthread_join(t5, NULL);

// 7. Dọn dẹp tài nguyên trước khi thoát (Cleanup)
    mq_close(mq_limit);
    mq_close(mq_pzem_oled);
    mq_close(mq_pzem_mqtt);
    mq_close(mq_pzem_led);
    mq_unlink(QUEUE_LIMIT);
    mq_unlink(QUEUE_PZEM_OLED);
    mq_unlink(QUEUE_PZEM_MQTT);
    mq_unlink(QUEUE_PZEM_LED);
    close(fd_oled);
    // Hủy Mutex
    pthread_mutex_destroy(&lock_threshold);
    return 0;
}