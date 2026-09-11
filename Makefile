# 호스트별 경로는 저장소에 넣지 않는다. 저장소 루트의 `.env`(gitignore 대상)에서
# 읽는다 — `cp .env.example .env` 로 시작한다. `make KERNEL_SRC=...` 로 넘기면
# 명령줄이 이긴다. 래퍼를 거치면 환경변수가 .env 를 이기지만, make 를 직접 부르면
# make 가 .env 를 makefile 소스로 읽어 .env 가 환경변수를 이긴다.
#   KERNEL_SRC    : 커널 소스 트리 (arch/<arch>/configs 가 있는 쪽)
#   KBUILD_OUTPUT : 소스와 빌드가 나뉜 트리의 빌드 디렉터리 (.config 와
#                   include/config/auto.conf 가 있는 쪽). 결합형 트리
#                   (배포판 linux-headers 등)면 비워 둔다.
ifeq ($(KERNELRELEASE),)
-include .env
endif

KBUILD_O := $(if $(strip $(KBUILD_OUTPUT)),O=$(strip $(KBUILD_OUTPUT)))

ifeq ($(KERNELRELEASE),)
ifeq ($(strip $(KERNEL_SRC)),)
$(error KERNEL_SRC 가 비었다 — `cp .env.example .env` 로 채우거나 make KERNEL_SRC=... 로 넘긴다)
endif
endif

# 아래는 kbuild 가 이 파일을 외부 모듈 kbuild 파일로 다시 읽을 때도 필요하다.
# 위 블록들을 KERNELRELEASE 로 감싼 이유 — 그때는 KERNEL_SRC 가 보이지 않는다
# (makefile 변수는 sub-make 로 export 되지 않는다). 감싸지 않으면 bare make 와
# Yocto devshell 의 `make -C <커널> M=<여기> modules` 가 가드에서 죽는다.
obj-m += max9296.o

all:
	$(MAKE) -C $(KERNEL_SRC) $(KBUILD_O) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) $(KBUILD_O) M=$(PWD) clean
