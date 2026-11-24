/* 커널 로그 및 기본 커널 함수 선언. 나머지 헤더는 장치, IRQ, FIFO, 스레드, 동기화 및 타이머 API를 제공한다. */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/err.h>
#include <linux/string.h>

// 이벤트 한 줄 크기 및 스위치 디바운스 시간.
#define DEV_MAJOR_NUMBER 241
#define DEV_NAME "guard"
#define LINE_SIZE 64
#define DEBOUNCE_MS 200

// 자원 확보 개수는 초기화 실패 시 정리에 사용
static const int led_gpio[] = {23, 24, 25, 1};
static const int input_gpio[] = {4, 17, 27, 22, 7};
static int irqs[5], ids[] = {0, 1, 2, 3, 4}; // IRQ 번호와 핸들러에 전달할 장치 ID
static int leds_owned, inputs_owned, irqs_owned; // 성공적으로 확보한 자원 개수 
static unsigned long last_sw[4], start_time; // 스위치별 마지막 입력 시각, 모듈 적재 시각
static bool seen_sw[4]; // 첫 입력은 디바운스 검사 없이 허용

// 경보 시스템의 네 상태와 FSM에 전달할 입력 종류
enum guard_state { DISARMED, ARMING, ARMED, ALARM };
enum input_kind { IN_SW, IN_PIR, IN_ARM, IN_DISARM, IN_PANIC, IN_TIMER };

// 입력 종류, 스위치 번호, 타이머 세대 번호.
struct input { 
	enum input_kind kind; 
	int sw; 
	unsigned int generation;
};
// 사용자에게 전달할 문자열을 보관
struct event_line {
	char text[LINE_SIZE]; 
};

// 상태 문자열, 입력 FIFO(128개), 이벤트 FIFO(32줄), 공유 데이터 보호 락 및 대기 큐. read_lock은 여러 reader의 이벤트 소비를 직렬화
static const char * const state_name[] = {"DISARMED", "ARMING", "ARMED", "ALARM"};
/* kfifo_exam.c와 동일한 바이트 FIFO. 각 레코드 크기만큼 입출력한다. */
static struct kfifo input_fifo;
static struct kfifo event_fifo;
static bool input_allocated, event_allocated;

#define INPUT_CAPACITY 128
#define EVENT_CAPACITY 32

static DEFINE_SPINLOCK(guard_lock);
static DEFINE_MUTEX(read_lock);
static DECLARE_WAIT_QUEUE_HEAD(input_wait);
static DECLARE_WAIT_QUEUE_HEAD(event_wait);
// 커널 스레드와 LED 타이머, 현재 상태 및 표시 단계. generation은 취소된 이전 타이머 이벤트를 구별
static struct task_struct *fsm; // 입력을 처리하는 커널 스레드
static struct timer_list led_timer;
static enum guard_state state = DISARMED;
static unsigned int generation, phase; // 상태 전이 세대 번호, LED 표시 단계
static unsigned long arm_start; // ARMING 시작 jiffies
static bool failure_flash; // 해제 실패 전체 점멸 진행 여부
static unsigned int code_pos; // FSM 스레드만 접근

// 4비트 마스크의 최상위 비트부터 LED0~3에 대응하여 출력
static void show_leds(unsigned int mask){
	int i;
	for (i = 0; i < 4; i++) {
        if (mask & (8 >> i)) //1000 0100 0010 0001
            gpio_set_value(led_gpio[i], 1);
        else
            gpio_set_value(led_gpio[i], 0);
    }
}

// IRQ, write, 타이머에서 입력을 저장하고 FSM을 깨움. ARMING 완료 입력을 위해 일반 입력은 마지막 한 칸을 남긴다.
static bool queue_input(struct input in){
	unsigned long flags;
	bool ok = false;
	spin_lock_irqsave(&guard_lock, flags);
	// kfifo_alloc은 크기를 2의 거듭제곱으로 올리므로 논리 용량도 제한
	if (kfifo_len(&input_fifo) / sizeof(in) < INPUT_CAPACITY - (in.kind == IN_TIMER ? 0 : 1))
		ok = kfifo_in(&input_fifo, &in, sizeof(in)) == sizeof(in);
	spin_unlock_irqrestore(&guard_lock, flags);
	if (ok)
		wake_up_interruptible(&input_wait);
	else
		printk(KERN_INFO "[guard] input fifo full, dropped\n");
	return ok;
}

// 입력 FIFO가 비어 있지 않은지 락으로 보호하여 확인한다. FSM의 대기 조건
static bool input_ready(void){
	unsigned long flags;
	bool ready;
	spin_lock_irqsave(&guard_lock, flags);
	ready = !kfifo_is_empty(&input_fifo);
	spin_unlock_irqrestore(&guard_lock, flags);
	return ready;
}

// 이벤트 FIFO에 읽을 줄이 있는지 확인한다. read의 대기 조건
static bool event_ready(void){
	unsigned long flags;
	bool ready;
	spin_lock_irqsave(&guard_lock, flags);
	ready = !kfifo_is_empty(&event_fifo);
	spin_unlock_irqrestore(&guard_lock, flags);
	return ready;
}

// 모듈 적재 이후 경과 시간을 붙여 이벤트 한 줄을 저장하고 reader를 깨운다. FIFO가 가득 차면 새 이벤트를 버린다.
static void emit_event(const char *body){
	struct event_line line = { { 0 } };
	unsigned long flags;
	bool ok;
	scnprintf(line.text, sizeof(line.text), "%u %s\n",
			  jiffies_to_msecs(jiffies - start_time), body);
	spin_lock_irqsave(&guard_lock, flags);
	ok = kfifo_in(&event_fifo, &line, sizeof(line)) == sizeof(line);
	spin_unlock_irqrestore(&guard_lock, flags);
	if (ok)
		wake_up_interruptible(&event_wait); //이벤트 저장
	else
		printk(KERN_INFO "[guard] event fifo full, dropped\n");
}

/* dev_id로 SW/PIR을 구분한다. SW는 200ms 이내 중복 입력을 무시하고 입력만 FIFO에 넣는다. 상태를 직접 변경하지 않는다. */
static irqreturn_t guard_irq(int irq, void *dev_id){
	int n = *(int *)dev_id;
	unsigned long flags, now = jiffies;
	bool ignore = false;
	struct input in = { .kind = n == 4 ? IN_PIR : IN_SW, .sw = n };
	if (n < 4) {
		spin_lock_irqsave(&guard_lock, flags);
		if (seen_sw[n] && time_before(now, last_sw[n] + msecs_to_jiffies(DEBOUNCE_MS)))
			ignore = true;
		else { seen_sw[n] = true; last_sw[n] = now; }
		spin_unlock_irqrestore(&guard_lock, flags);
	}
	if (!ignore)
		queue_input(in);
	return IRQ_HANDLED;
}

/* 타이머 콜백: ARMING 카운트다운과 ALARM 교대/실패 점멸을 처리한다. 5초 완료 시 상태 변경 대신 IN_TIMER를 전달한다. */
static void led_tick(struct timer_list *timer){
	unsigned long flags, next = 0;
	unsigned int mask = 0;
	bool output = false, rearm = false, done = false;
	struct input in = { .kind = IN_TIMER };
	spin_lock_irqsave(&guard_lock, flags);
	/* 1초마다 왼쪽 LED부터 하나씩 끈다. */
	if (state == ARMING) {
		++phase;
		if (phase >= 5) {
			in.generation = generation;
			done = true;
		} else {
			mask = 15U >> phase;
			output = rearm = true;
			next = arm_start + msecs_to_jiffies(1000 * (phase + 1));
		}
	/* 6단계(ON/OFF 3쌍)가 끝나면 교대 점멸로 복귀한다. */
	} else if (state == ALARM) {
		++phase;
		if (failure_flash && phase >= 6) {
			failure_flash = false;
			phase = 0;
		}
		if (failure_flash)
			mask = (phase & 1) ? 0 : 15;
		else
			mask = (phase & 1) ? 5 : 10;
		output = rearm = true;
		next = jiffies + msecs_to_jiffies(200);
	}
	spin_unlock_irqrestore(&guard_lock, flags);
	/* GPIO 출력과 타이머 재등록은 spinlock 밖에서 실행한다. */
	if (output) show_leds(mask);
	/* led_timer.c와 같이 만료 시간을 설정한 뒤 재등록한다. */
	if (rearm) { led_timer.expires = next; add_timer(&led_timer); }
	if (done) queue_input(in);
}

/* FSM 전용 상태 변경 함수. 이전 타이머가 끝날 때까지 동기적으로 기다린 뒤 상태와 표시를 초기화하고 이벤트를 기록한다. */
static void change_state(enum guard_state next, const char *reason){
	unsigned long flags, first = 0;
	enum guard_state old;
	char body[32];
	del_timer_sync(&led_timer);
	spin_lock_irqsave(&guard_lock, flags);
	old = state;
	state = next;
	++generation;
	phase = 0;
	failure_flash = false;
	code_pos = 0;
	if (next == ARMING) {
		arm_start = jiffies;
		first = arm_start + msecs_to_jiffies(1000);
	} else if (next == ALARM) {
		first = jiffies + msecs_to_jiffies(200);
	}
	spin_unlock_irqrestore(&guard_lock, flags);
	show_leds(next == ARMING ? 15 : next == ARMED ? 1 : next == ALARM ? 10 : 0);
	if (next == ARMING || next == ALARM) {
		led_timer.expires = first;
		add_timer(&led_timer);
	}
	scnprintf(body, sizeof(body), "STATE %s", state_name[next]);
	emit_event(body);
	printk(KERN_INFO "[guard] state %s -> %s (by %s)\n", state_name[old], state_name[next], reason);
}

/* 해제 실패 시 기존 점멸을 중단하고 전체 LED 3회 점멸을 시작한다. 이후 타이머가 교대 점멸로 복귀한다. */
static void flash_failure(void){
	unsigned long flags;
	del_timer_sync(&led_timer);
	spin_lock_irqsave(&guard_lock, flags);
	failure_flash = true;
	phase = 0;
	spin_unlock_irqrestore(&guard_lock, flags);
	show_leds(15);
	led_timer.expires = jiffies + msecs_to_jiffies(200);
	add_timer(&led_timer);
}

/* 입력 종류와 현재 상태를 비교해 상태 전이를 결정한다. PIR/SW 기록과 해제 순서 검사를 담당한다. */
static void handle_input(struct input in){
	unsigned long flags;
	enum guard_state current_state;
	unsigned int gen;
	char body[16];
	spin_lock_irqsave(&guard_lock, flags);
	current_state = state;
	gen = generation;
	spin_unlock_irqrestore(&guard_lock, flags);
	switch (in.kind) {
	/* PIR은 모든 상태에서 기록하되 ARMED에서만 경보를 시작한다. */
	case IN_PIR:
		emit_event("PIR");
		if (current_state == ARMED) change_state(ALARM, "PIR");
		break;
	/* SW3은 시작/취소, SW0~2는 ALARM 해제 순서에 사용한다. */
	case IN_SW:
		scnprintf(body, sizeof(body), "SW %d", in.sw);
		emit_event(body);
		if (in.sw == 3) {
			if (current_state == DISARMED) change_state(ARMING, "SW3");
			else if (current_state == ARMING) change_state(DISARMED, "SW3");
		} else if (current_state == ALARM) {
			if (in.sw == code_pos) {
				if (++code_pos == 3) {
					emit_event("CODE_OK");
					change_state(DISARMED, "CODE_OK");
				}
			} else {
				code_pos = 0;
				emit_event("CODE_FAIL");
				flash_failure();
			}
		}
		break;
	/* 사용자 ARM 명령: 해제 상태에서만 경보 준비를 시작한다. */
	case IN_ARM:
		if (current_state == DISARMED) change_state(ARMING, "ARM");
		break;
	/* 사용자 DISARM 명령: 모든 활성 상태를 해제한다. */
	case IN_DISARM:
		if (current_state != DISARMED) change_state(DISARMED, "DISARM");
		break;
	/* 사용자 PANIC 명령: 감지 대기 상태에서 강제로 경보를 발생시킨다. */
	case IN_PANIC:
		if (current_state == ARMED) change_state(ALARM, "PANIC");
		break;
	/* 이전 ARMING에서 남은 완료 이벤트는 세대 번호로 무시한다. */
	case IN_TIMER:
		if (current_state == ARMING && in.generation == gen) change_state(ARMED, "TIMER");
		break;
	}
}

/* 입력이 없으면 wait queue에서 잠들고, 입력이 들어오면 FIFO에서 하나 꺼내 처리한다. 종료 요청도 대기 조건에 포함한다. */
static int guard_fsm(void *unused){
	struct input in;
	unsigned long flags;
	bool got;
	while (!kthread_should_stop()) {
		if (wait_event_interruptible(input_wait, kthread_should_stop() || input_ready()))
			continue;
		if (kthread_should_stop()) break;
		spin_lock_irqsave(&guard_lock, flags);
		got = kfifo_out(&input_fifo, &in, sizeof(in)) == sizeof(in);
		spin_unlock_irqrestore(&guard_lock, flags);
		if (got) handle_input(in);
	}
	return 0;
}

/* 이벤트 한 줄을 사용자 공간에 전달한다. 이벤트가 없으면 블로킹하고 O_NONBLOCK이면 -EAGAIN을 반환한다. */
static ssize_t guard_read(struct file *file, char __user *buf, size_t count, loff_t *pos){
	struct event_line line;
	unsigned long flags;
	ssize_t ret;
	size_t len;
	bool got;
	if (!count) return 0;
	/* 대기 중에는 mutex를 잡지 않는다. 이벤트 소비자만 직렬화한다. */
	for (;;) {
		if (file->f_flags & O_NONBLOCK) {
			if (!event_ready()) return -EAGAIN;
		} else {
			ret = wait_event_interruptible(event_wait, event_ready());
			if (ret) return ret;
		}
		ret = mutex_lock_interruptible(&read_lock);
		if (ret) return ret;
		spin_lock_irqsave(&guard_lock, flags);
		got = kfifo_out_peek(&event_fifo, &line, sizeof(line)) == sizeof(line);
		spin_unlock_irqrestore(&guard_lock, flags);
		if (got) break;
		mutex_unlock(&read_lock);
	}

	/* 복사에 성공해야 FIFO에서 제거한다. 실패하면 다음 read에서 재시도할 수 있다. */
	len = strlen(line.text);
	if (count < len) ret = -EMSGSIZE;
	else if (copy_to_user(buf, line.text, len)) ret = -EFAULT;
	else {
		spin_lock_irqsave(&guard_lock, flags);
		/* 바이트 FIFO이므로 한 줄 전체를 제거해야 한다. */
		got = kfifo_out(&event_fifo, &line, sizeof(line)) == sizeof(line);
		spin_unlock_irqrestore(&guard_lock, flags);
		ret = len;
	}
	mutex_unlock(&read_lock);
	return ret;
}

/* 사용자 문자열을 복사하고 끝 개행을 제거한다. ARM/DISARM/PANIC만 입력 FIFO에 전달하며 잘못된 명령은 -EINVAL을 반환한다. */
static ssize_t guard_write(struct file *file, const char __user *buf, size_t count, loff_t *pos){
	char cmd[16];
	size_t n = count;
	struct input in = { 0 };
	if (!count || count >= sizeof(cmd)) return -EINVAL;
	if (copy_from_user(cmd, buf, count)) return -EFAULT;
	if (memchr(cmd, '\0', count)) return -EINVAL;
	if (n && cmd[n-1] == '\n') --n;
	cmd[n] = '\0';
	if (!strcmp(cmd, "ARM")) in.kind = IN_ARM;
	else if (!strcmp(cmd, "DISARM")) in.kind = IN_DISARM;
	else if (!strcmp(cmd, "PANIC")) in.kind = IN_PANIC;
	else return -EINVAL;
	if (!queue_input(in)) return -EAGAIN;
	return count;
}

/* 문자 장치의 시스템 콜 연결. THIS_MODULE은 열린 파일이 있는 동안 모듈 제거를 막는다. */
static const struct file_operations guard_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.read = guard_read,
	.write = guard_write,
};

// IRQ 해제 → FSM 종료 → 타이머 동기 제거 → GPIO 해제 순서로 자원을 반환.
static void release_hardware(void){

	while (irqs_owned) {
		--irqs_owned; 
		free_irq(irqs[irqs_owned], &ids[irqs_owned]); 
	}

	if (fsm) {
		kthread_stop(fsm); 
		fsm = NULL; 
	}

	del_timer_sync(&led_timer);
	// 스레드와 타이머가 종료된 뒤 FIFO 메모리를 반환
	if (event_allocated) { 
		kfifo_free(&event_fifo); 
		event_allocated = false; 
	}

	if (input_allocated) { 
		kfifo_free(&input_fifo); 
		input_allocated = false; 
	}

	while (inputs_owned) gpio_free(input_gpio[--inputs_owned]);

	while (leds_owned) { 
		--leds_owned; 
		gpio_set_value(led_gpio[leds_owned], 0); //led 끄기
		gpio_free(led_gpio[leds_owned]); 
	}
}

// 모듈 적재 시 GPIO, 타이머, IRQ, FSM, 문자 장치를 초기화. 실패 시 이미 확보한 자원만 반환
static int guard_init(void){
	int i, ret;
	start_time = jiffies;
	timer_setup(&led_timer, led_tick, 0);
	// 적재 시 할당하고 제거 시 반환한다.
	ret = kfifo_alloc(&input_fifo, INPUT_CAPACITY * sizeof(struct input), GFP_KERNEL);
	if (ret) goto fail;
	input_allocated = true;
	ret = kfifo_alloc(&event_fifo, EVENT_CAPACITY * sizeof(struct event_line), GFP_KERNEL);
	if (ret) goto fail;
	event_allocated = true;
	for (i = 0; i < 4; ++i) {
		ret = gpio_request(led_gpio[i], "guard_led");
		if (ret) goto fail;
		++leds_owned;
		if (gpio_cansleep(led_gpio[i])) { ret = -EINVAL; goto fail; }
		ret = gpio_direction_output(led_gpio[i], 0);
		if (ret) goto fail;
	}
	for (i = 0; i < 5; ++i) {
		ret = gpio_request(input_gpio[i], "guard_input");
		if (ret) goto fail;
		++inputs_owned;
		ret = gpio_direction_input(input_gpio[i]);
		if (ret) goto fail;
		irqs[i] = gpio_to_irq(input_gpio[i]);
		if (irqs[i] < 0) { ret = irqs[i]; goto fail; }
	}
	fsm = kthread_run(guard_fsm, NULL, "guard_fsm");
	if (IS_ERR(fsm)) { ret = PTR_ERR(fsm); fsm = NULL; goto fail; }
	// SW는 상승 에지, PIR은 하강 에지에서 IRQ 발생
	for (i = 0; i < 5; ++i) {
		ret = request_irq(irqs[i], guard_irq, i == 4 ? IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING, "guard", &ids[i]);
		if (ret) goto fail;
		++irqs_owned;
	}
	ret = register_chrdev(DEV_MAJOR_NUMBER, DEV_NAME, &guard_fops);
	if (ret < 0) goto fail;
	printk(KERN_INFO "[guard] loaded\n");
	return 0;
fail:
	release_hardware();
	return ret;
}

/* 모듈 제거 시 문자 장치를 등록 해제하고 모든 하드웨어 자원을 정리한다. */
static void guard_exit(void){
	unregister_chrdev(DEV_MAJOR_NUMBER, DEV_NAME);
	release_hardware();
	printk(KERN_INFO "[guard] unloaded\n");
}
/* 커널 모듈의 진입/종료 함수 및 GPL 라이선스를 등록한다. */
module_init(guard_init);
module_exit(guard_exit);
MODULE_LICENSE("GPL");
