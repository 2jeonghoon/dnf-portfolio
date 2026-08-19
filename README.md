# io_uring + MySQL Game Server Portfolio

네오플 [던전앤파이터] 서버 프로그래머 직무에 맞춰 C++ 네트워크, 비동기 처리, 데이터베이스 연동 역량을 보여주기 위한 작은 게임 서버 예제입니다.

## What It Shows

- `io_uring` 기반 TCP accept/recv/send 이벤트 루프
- fd 재사용으로 인한 stale completion 오동작을 막는 session generation 검증
- sector grid 기반 interest management와 주변 플레이어 동기화
- 이동/공격 액션을 주변 sector에만 broadcast하는 MMORPG식 월드 이벤트
- MySQL C API 기반 prepared statement, connection pool, transaction 처리
- DB blocking 호출을 event loop에서 분리하는 worker pool + `eventfd` completion bridge
- 계정, 캐릭터, 인벤토리 슬롯 저장/조회로 구성한 게임 서버 데이터 모델

## Build

필요 패키지는 C++17 컴파일러, CMake, `liburing`, MySQL client 개발 헤더입니다.

```bash
cmake -S . -B build
cmake --build build
```

## Database

```bash
mysql -u root -p < db/schema.sql
```

기본 DB 이름은 `dungeon_portfolio`입니다. 실행 시 환경변수로 접속 정보를 바꿀 수 있습니다.

```bash
MYSQL_HOST=127.0.0.1 \
MYSQL_PORT=3306 \
MYSQL_USER=root \
MYSQL_PASSWORD=your_password \
MYSQL_DATABASE=dungeon_portfolio \
MYSQL_POOL_SIZE=4 \
SERVER_PORT=9090 \
./build/dungeon_portfolio_server
```

## Protocol

줄 단위 텍스트 프로토콜입니다. 한 연결에서 여러 요청을 연속으로 보낼 수 있고, DB 작업은 `PENDING` 이후 `DONE` 또는 `FAIL`로 완료됩니다.

```text
REGISTER <account> <display_name>
LOGIN <account>
SAVE <account> <character> <level> <gold>
LOAD <account>
ENTER <account> <character> <x> <y>
MOVE <x> <y>
ATTACK <skill> <target>
SAVE_ITEM <account> <slot> <item_id> <count>
LOAD_INVENTORY <account>
QUIT
```

Example:

```bash
nc 127.0.0.1 9090
REGISTER fighter01 BladeMaster
LOGIN fighter01
SAVE fighter01 main 110 3000000
LOAD fighter01
ENTER fighter01 main 120 80
MOVE 180 90
ATTACK slash goblin_01
SAVE_ITEM fighter01 0 1002001 3
LOAD_INVENTORY fighter01
QUIT
```

응답 예:

```text
HELLO commands=REGISTER,LOGIN,SAVE,LOAD,QUIT
PENDING 1 REGISTER
DONE 1 OK REGISTER account=fighter01 display_name=BladeMaster
PENDING 2 LOGIN
DONE 2 OK LOGIN player_id=1 account=fighter01 display_name=BladeMaster
WORLD ENTERED main x=120 y=80 sector=1,0
WORLD MOVED main x=180 y=90 sector=1,0
WORLD ATTACK skill=slash target=goblin_01
```

## Tests

`tests/`에 소켓 기반 통합 테스트(`pytest`)가 있습니다. 실제로 서버 프로세스를 띄우고 실제 MySQL에 연결해 REGISTER/LOGIN/SAVE/LOAD/인벤토리/월드 브로드캐스트/프로토콜 에러 시나리오를 검증합니다.

```bash
pip install -r tests/requirements.txt

# 테스트 전용 스키마 준비 (최초 1회, 기본 DB 이름은 dungeon_portfolio_test)
sed 's/dungeon_portfolio/dungeon_portfolio_test/g' db/schema.sql | mysql -u root -p

pytest tests/
```

테스트 fixture가 `build/dungeon_portfolio_server`를 자동으로 빌드하고, `SERVER_PORT=19090`(기본 9090과 충돌 방지)으로 서버를 띄운 뒤 각 테스트가 끝나면 종료합니다. DB 접속 정보는 `TEST_MYSQL_HOST`, `TEST_MYSQL_PORT`, `TEST_MYSQL_USER`, `TEST_MYSQL_PASSWORD`, `TEST_MYSQL_DATABASE`, 서버 포트는 `TEST_SERVER_PORT` 환경변수로 바꿀 수 있습니다.

## Code Map

- `src/server.cpp`: `io_uring` 이벤트 루프, 세션 관리, network completion 처리
- `src/world.cpp`: sector 기반 player visibility, 이동/공격 event fan-out
- `src/database.cpp`: MySQL connection pool, prepared statement, transaction 처리
- `src/protocol.cpp`: line protocol parsing and validation
- `db/schema.sql`: players, characters, inventory_items InnoDB schema
- `tests/`: pytest 기반 소켓 통합 테스트, `conftest.py`가 서버 기동/DB 초기화 담당
