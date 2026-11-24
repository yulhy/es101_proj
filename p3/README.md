# 문제 3 — Source.zip 기반 PIR 경보 시스템

## 참고한 수업 코드
- Source/module/kfifo_exam/kfifo_exam.c: struct kfifo, kfifo_alloc/in/out/free
- Source/module/led_timer/led_timer.c: timer_setup, expires, add_timer, LED GPIO 번호
- Source/module/switch_interrupt/switch_interrupt.c: SW GPIO, 상승 에지
- Source/driver/pir_driver/pir_driver.c: PIR GPIO 7, 하강 에지, wait queue, 문자 장치 등록
- Source/module/kthread_exam/kthread_exam.c: kthread_run/should_stop/stop
- Source/driver/simple_buffer/simple_buffer.c: copy_from_user/to_user
- Source/native/pir_native/pir_native.c 및 simple_buffer_native.c: open/read/write/close
- Source/driver/pir_driver/Makefile: KDIR, PWD, SUBDIRS

## 수업 예제와 다른 부분
- del_timer_sync: 콜백 종료까지 기다려 이전 상태의 LED 출력 및 제거 후 접근을 방지한다.
- gpio_set_value: 초기화 시 출력 방향을 설정한 뒤 값만 변경한다. 타이머에서는 잠드는 GPIO를 사용할 수 없어 cansleep 검사도 둔다(커널 버전에 따라 API 분기).
- kfifo_out_peek: 사용자 공간 복사 실패 시 이벤트를 잃지 않도록 먼저 조회한 후 성공 시 전체 레코드를 제거한다.
- spin_lock_irqsave: IRQ/FSM/read/write/timer 간 공유 데이터 보호. read_lock은 여러 reader의 소비 순서를 보호하며 spinlock 밖에서 사용한다.
- wait_event_interruptible 조건은 실제 FIFO 데이터 존재 여부 및 스레드 종료 요청을 검사한다. 수업 PIR 예제의 TASK_INTERRUPTIBLE 상수 조건은 그대로 복제하지 않는다.
- GPIO/IRQ는 open/release가 아니라 모듈 init/exit에서 확보/반환한다. 장치 파일을 열지 않아도 동작해야 하기 때문이다.
- read/write 반환형은 ssize_t, 사용자 포인터는 __user, 반환값은 실제 전송 길이 또는 음수 오류 코드이다. copy_*_user의 0이 아닌 반환값은 오류로 처리한다.
- pthread는 Source.zip에서 사용 예제를 확인하지 못했으므로 별도로 구현했다.
- 입력 FIFO 논리 용량 128개, 이벤트 FIFO 32줄, 디바운스 200ms. 입력 FIFO의 한 칸은 타이머 완료 이벤트용으로 예약한다.
- Makefile은 수업 예제의 SUBDIRS와 최신 커널의 M을 함께 전달한다.

## 실행
```sh
make
sudo insmod guard.ko
sudo mknod /dev/guard c 241 0 # 기존 장치 파일이 없을 때만
sudo chmod 666 /dev/guard
./guard_ctl
```
arm / disarm / panic / quit 명령을 지원한다. 이벤트는 guard.log에 추가 기록한다.

별도 터미널에서 cat /dev/guard 및 echo ARM > /dev/guard 등의 명령으로 검사할 수 있다. 이벤트 FIFO는 소비형이므로 guard_ctl과 cat은 동시에 사용하지 않는 것이 좋다.
sudo rmmod guard.ko

## 검증 및 제거
1. 초기 LED OFF, ARM 후 5초 카운트다운과 LED3 ON 확인.
2. ARMING 중 SW3/DISARM 취소 확인.
3. ARMED에서 PIR 감지/PANIC 시 200ms 교대 점멸 확인.
4. SW0-1-1 실패 점멸 3회, SW0-1-2 성공 해제 확인.
5. 이벤트가 없을 때 read가 블로킹되고 quit으로 종료되는지 확인.
6. 사용자 프로그램 및 cat 종료 후 sudo rmmod guard, sudo rm /dev/guard. dmesg에서 경고 여부 확인.

THIS_MODULE을 유지하므로 열린 파일이 있는 상태에서는 정상 rmmod가 거부된다. 강제 제거하지 않는다. 과제의 열린 cat 상태 제거 채점 조건이 실제 제거 성공을 요구한다면 별도 확인이 필요하다.

## AI 사용 내역
ChatGPT를 사용하여 제공된 수업 예제와 기존 구현의 API를 대조하고, FIFO 및 타이머 방식 수정, 한글 주석 작성을 수행했다.
