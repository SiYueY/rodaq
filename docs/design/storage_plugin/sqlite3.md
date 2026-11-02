# RoDAQ SQLite3 Storage Plugin Mapping Specification

**Version:** v0.5
**Status:** Future Storage Mapping / Architecture Validation
**Scope:** Future SQLite3 Storage Plugin mapped against the RoDAQ V1 Canonical/Storage contracts
**Implementation Scope:** Designed only; not part of V1 implementation or release gate

**Normative Dependencies:**

- RoDAQ Canonical Data Model v0.9
- RoDAQ V1 Canonical Profile v0.6
- RoDAQ System Architecture Design Specification v0.6
- RoDAQ Runtime Design Specification v0.6
- RoDAQ Payload Design Specification v0.5
- RoDAQ Admission Design Specification v0.5
- RoDAQ Storage Plugin Design Specification v0.5
- RoDAQ V1 Plugin System Design — current Freeze Candidate

---

# 1. 目标与映射策略

本文定义 RoDAQ V1 Canonical State 到 SQLite3 Artifact 的正式 full-fidelity Storage Mapping。

SQLite3 当前属于：

> **future / designed Storage target，用于 architecture validation；不属于 V1 implementation / release gate。**

当前 Ubuntu 22.04 开发机 SQLite library 为 3.37.2，可作为 deferred implementation baseline。设计只依赖 SQLite3 C API 中稳定的：

```text
prepared statement
bind / step / reset
transaction
PRAGMA
WAL / journal
limit / error code
```

能力，不依赖 rosbag2_storage_sqlite3 自身。后者用于参考真实机器人记录场景中的 pragma、性能与 crash-resilience 取舍。

它不属于当前 V1 的任何 implementation milestone；文档继续维护的目的，是证明 generic Storage Contract 对 relational/transaction backend 仍成立，并为后续版本提供实现基线。

SQLite3 与 MCAP 的价值不同：

```text
MCAP
    → append/container/channel/message model

SQLite3
    → relational/transaction/query model
```

因此 SQLite3 是非常重要的第二 Storage evidence：

> 用来验证 Storage Contract 是否真正 backend-independent，而不是只适合 MCAP。

V1 SQLite Mapping 目标：

```text
exact Canonical scalar preservation
explicit Descriptor graph integrity
transaction-bounded persistence completion
bounded Payload lifetime
queryable Record/Descriptor relationship
```

不得把以下 SQLite 物理概念变成 Canonical semantics：

```text
rowid
signed INTEGER convenience
physical row order
TEXT collation
WAL file layout
```

## 1.1 SQLite Scalar Constraint

SQLite `INTEGER` 是 signed 64-bit。

而 Profile v0.5 以下字段是完整 `uint64` domain：

```text
Canonical IDs
AcceptanceSequence
SourceSequence
Clock resolution numerator / denominator
```

因此：

```text
INT64_MAX + 1
...
UINT64_MAX
```

不能无损存入普通 SQLite INTEGER。

V1 统一使用：

```text
U64BE(value)
= fixed 8-byte big-endian BLOB
```

保存这些字段。

优点：

```text
完整 uint64 round-trip
fixed-width bytewise order == unsigned numeric order
no signed reinterpretation
```

`ClockReading` 本身就是 Profile `int64`：

```text
→ SQLite INTEGER
```

Canonical UTF-8 Identifier 以及 opaque bytes：

> 使用 BLOB 保存 exact bytes。

这样不会让 SQLite collation/encoding conversion 参与 Canonical equality。

---

## 1.2 参考实现与工程取舍

SQLite3 Storage 主要参考：

```text
ros2/rosbag2_storage_sqlite3
https://github.com/ros2/rosbag2/tree/rolling/rosbag2_storage_sqlite3

eProsima DDS Record & Replay SQL output
https://github.com/eProsima/DDS-Record-Replay

SQLite official C API / WAL / transaction semantics
```

### rosbag2_storage_sqlite3 的经验

rosbag2 SQLite backend 证明 `.db3` 作为机器人本地记录格式非常成熟，尤其适合：

```text
简单部署
SQL query
按 topic/time 分析
无额外服务进程
```

它也暴露一个重要事实：

> 为了吞吐，默认 SQLite pragma 往往会牺牲 crash resistance。

rosbag2_storage_sqlite3 因此提供 storage config 和 `resilient` preset。

RoDAQ 应直接吸收这个工程经验：

```text
performance profile
```

和：

```text
crash-resilience profile
```

不能被混成一个“默认一定安全”的数据库配置。

### DDS Record & Replay SQL schema 的经验

DDS Record & Replay 当前 SQL output 已经包含：

```text
Types
Topics
Messages
Partitions
TopicsPartitions
MessagesPartitions
```

其中 Messages 保存：

```text
writer_guid
sequence_number
CDR / JSON data
Topic/type
log_time
publish_time
```

这进一步验证：

```text
Source identity
Source sequence
multiple timestamps
raw serialized bytes
```

在 SQLite backend 中是现实且可查询的数据。

RoDAQ 与其主要区别是：

> RoDAQ 必须保存完整 Canonical scalar domain，而不是以某个 recorder-specific SQL schema 为中心。

例如 Profile v0.5 的：

```text
uint64 AcceptanceSequence
uint64 Canonical IDs
```

不能因为 SQLite INTEGER 是 signed 64-bit 就缩窄。

因此本设计采用 fixed 8-byte big-endian BLOB (`U64BE`)。

### 为什么不用 TEXT 存 uint64

使用 decimal TEXT 也可以 round-trip uint64，但会带来：

```text
string parse cost
canonical formatting rule
lexicographic order != numeric order unless zero-padded
more storage bytes
```

固定 8-byte big-endian BLOB 更简单：

```text
exact
compact
bytewise order == unsigned numeric order
```

这也是 V1 明确冻结的 Storage representation。

### transaction 是真实 persistence batch

SQLite transaction 提供了比 MCAP Chunk 更明确的 batch commit boundary：

```text
sqlite3_step(SQLITE_DONE)
    != PersistenceSucceeded

COMMIT success
    → batch persistence terminal success
```

所以 SQLite backend 是对 generic `PersistenceSucceeded` contract 的重要反证：

> 如果 Runtime 把“backend 接收了一条 write 调用”直接当 persistence success，会在 SQLite 上明显错误。

### WAL / synchronous 不是 Canonical semantics

SQLite 的：

```text
journal_mode
synchronous
checkpoint policy
```

决定 crash/power-loss 行为和性能。

它们必须属于 Storage configuration / durability policy，而不是 Canonical State。

同一个 Canonical corpus：

```text
SQLite WAL + NORMAL
SQLite rollback journal + FULL
```

都应该 decode 成相同 Canonical State，只是 failure/durability guarantee 不同。

### V1 不复制 rosbag2 schema

rosbag2 的经典 schema 更偏：

```text
Topic table
Message table(timestamp + data)
```

RoDAQ 还必须有：

```text
Descriptor graph
Source presence
full uint64 sequence
multiple timestamp slots
```

所以使用独立 relational schema 是必要的，不是为了与 rosbag2 不同而不同。

---

# 2. Relational Projection

## 2.1 Artifact Metadata

V1 database 有一个单例 Artifact metadata row。

最小 schema：

```sql
CREATE TABLE artifact (
    singleton_id        INTEGER PRIMARY KEY CHECK(singleton_id = 1),
    format              TEXT NOT NULL,
    canonical_model     TEXT NOT NULL,
    canonical_profile   TEXT NOT NULL,
    mapping_version     TEXT NOT NULL,
    state               TEXT NOT NULL
);
```

这些字段是 Storage metadata，不是 Canonical Identifier，因此这里使用 SQLite TEXT 没有 Canonical equality 问题。

logical value：

```text
format            = rodaq/sqlite/v1
canonical_model   = v0.9
canonical_profile = v0.5
mapping_version   = v0.3
state             = Incomplete | NormalComplete | optional diagnostic state
```

Prepare：

```text
state = Incomplete
```

Starting activation abort：

```text
state remains Incomplete / Aborted
```

数据库 connection 关闭成功：

> 不等于 NormalComplete。

## 2.2 Scalar Encoding

正式 helper semantics：

```text
U64BE(value)
    = exactly 8 bytes, big-endian unsigned

I64(value)
    = SQLite INTEGER / sqlite3_int64

Bytes(value)
    = SQLite BLOB preserving exact bytes
```

所有 U64BE column 必须同时约束 storage class 与长度：

```text
CHECK(typeof(value) = 'blob' AND length(value) = 8)
```

仅声明 column affinity 为 `BLOB` 不足以构成 full-fidelity correctness proof，因为 SQLite 是 dynamic typing。

optional U64BE：

```text
NULL = absent
8-byte zero = present(0)
```

因此：

```text
NULL
!= U64BE(0)
```

不得使用：

```text
0
UINT64_MAX
-1
```

作为 generic absence sentinel。

## 2.3 Descriptor Tables

V1 最小 relational model：

```text
artifact
schemas
clocks
timestamp_key_sets
timestamp_keys
stream_descriptors
records
```

实现可以增加 index / view / WITHOUT ROWID，但不得改变 logical mapping。

### schemas

```sql
CREATE TABLE schemas (
    schema_id           BLOB PRIMARY KEY
        CHECK(typeof(schema_id) = 'blob' AND length(schema_id) = 8),
    schema_format       BLOB NOT NULL CHECK(typeof(schema_format) = 'blob'),
    type_identifier     BLOB CHECK(type_identifier IS NULL OR typeof(type_identifier) = 'blob'),
    representation      BLOB NOT NULL CHECK(typeof(representation) = 'blob')
);
```

```text
type_identifier IS NULL
    → absent
```

### clocks

```sql
CREATE TABLE clocks (
    clock_id            BLOB PRIMARY KEY
        CHECK(typeof(clock_id) = 'blob' AND length(clock_id) = 8),
    resolution_num      BLOB NOT NULL
        CHECK(typeof(resolution_num) = 'blob' AND length(resolution_num) = 8),
    resolution_den      BLOB NOT NULL
        CHECK(typeof(resolution_den) = 'blob' AND length(resolution_den) = 8),
    clock_origin        BLOB NOT NULL CHECK(typeof(clock_origin) = 'blob')
);
```

resolution num/den：

```text
U64BE
> 0
reduced rational
```

### timestamp_key_sets

```sql
CREATE TABLE timestamp_key_sets (
    timestamp_key_set_id BLOB PRIMARY KEY
        CHECK(typeof(timestamp_key_set_id) = 'blob' AND length(timestamp_key_set_id) = 8),
    key_count INTEGER NOT NULL CHECK(key_count BETWEEN 1 AND 4)
);
```

### timestamp_keys

```sql
CREATE TABLE timestamp_keys (
    timestamp_key_set_id BLOB NOT NULL,
    slot                 INTEGER NOT NULL CHECK(slot BETWEEN 0 AND 3),
    role_identifier      BLOB NOT NULL CHECK(typeof(role_identifier) = 'blob'),
    clock_id             BLOB NOT NULL
        CHECK(typeof(clock_id) = 'blob' AND length(clock_id) = 8),
    PRIMARY KEY(timestamp_key_set_id, slot),
    FOREIGN KEY(timestamp_key_set_id)
        REFERENCES timestamp_key_sets(timestamp_key_set_id),
    FOREIGN KEY(clock_id)
        REFERENCES clocks(clock_id)
);
```

slot 只是 Storage projection。

Decode 必须恢复：

> same unordered Canonical TimestampKeySet。

### stream_descriptors

```sql
CREATE TABLE stream_descriptors (
    stream_descriptor_id BLOB PRIMARY KEY
        CHECK(typeof(stream_descriptor_id) = 'blob' AND length(stream_descriptor_id) = 8),
    stream_id             BLOB NOT NULL
        CHECK(typeof(stream_id) = 'blob' AND length(stream_id) = 8),
    stream_name           BLOB NOT NULL CHECK(typeof(stream_name) = 'blob'),
    schema_id             BLOB
        CHECK(schema_id IS NULL OR (typeof(schema_id) = 'blob' AND length(schema_id) = 8)),
    payload_encoding      BLOB NOT NULL CHECK(typeof(payload_encoding) = 'blob'),
    timestamp_key_set_id  BLOB NOT NULL
        CHECK(typeof(timestamp_key_set_id) = 'blob' AND length(timestamp_key_set_id) = 8),
    UNIQUE(stream_descriptor_id, stream_id),
    FOREIGN KEY(schema_id) REFERENCES schemas(schema_id),
    FOREIGN KEY(timestamp_key_set_id)
        REFERENCES timestamp_key_sets(timestamp_key_set_id)
);
```

注意：

```text
schema_id NULL
!=
schema_id U64BE(0)
```

## 2.4 Record Table

Record projection：

```sql
CREATE TABLE records (
    stream_id             BLOB NOT NULL
        CHECK(typeof(stream_id) = 'blob' AND length(stream_id) = 8),
    stream_descriptor_id  BLOB NOT NULL CHECK(typeof(stream_descriptor_id) = 'blob' AND length(stream_descriptor_id) = 8),
    acceptance_sequence   BLOB NOT NULL
        CHECK(typeof(acceptance_sequence) = 'blob' AND length(acceptance_sequence) = 8),

    source_id              BLOB
        CHECK(source_id IS NULL OR (typeof(source_id) = 'blob' AND length(source_id) > 0)),
    source_sequence        BLOB
        CHECK(source_sequence IS NULL OR
              (typeof(source_sequence) = 'blob' AND length(source_sequence) = 8)),

    timestamp_present_mask INTEGER NOT NULL
        CHECK(timestamp_present_mask BETWEEN 0 AND 15),
    ts0                    INTEGER,
    ts1                    INTEGER,
    ts2                    INTEGER,
    ts3                    INTEGER,

    payload                BLOB NOT NULL CHECK(typeof(payload) = 'blob'),

    PRIMARY KEY(stream_id, acceptance_sequence),

    CHECK(source_sequence IS NULL OR source_id IS NOT NULL),

    CHECK(
        ((timestamp_present_mask & 1) = 0 AND ts0 IS NULL) OR
        ((timestamp_present_mask & 1) != 0 AND ts0 IS NOT NULL AND typeof(ts0) = 'integer')
    ),
    CHECK(
        ((timestamp_present_mask & 2) = 0 AND ts1 IS NULL) OR
        ((timestamp_present_mask & 2) != 0 AND ts1 IS NOT NULL AND typeof(ts1) = 'integer')
    ),
    CHECK(
        ((timestamp_present_mask & 4) = 0 AND ts2 IS NULL) OR
        ((timestamp_present_mask & 4) != 0 AND ts2 IS NOT NULL AND typeof(ts2) = 'integer')
    ),
    CHECK(
        ((timestamp_present_mask & 8) = 0 AND ts3 IS NULL) OR
        ((timestamp_present_mask & 8) != 0 AND ts3 IS NOT NULL AND typeof(ts3) = 'integer')
    ),

    FOREIGN KEY(stream_descriptor_id, stream_id)
        REFERENCES stream_descriptors(stream_descriptor_id, stream_id)
);
```

### AcceptanceSequence

```text
U64BE
```

由于固定 8-byte big-endian：

```text
BLOB lexical order
==
unsigned uint64 numeric order
```

所以：

```sql
ORDER BY stream_id, acceptance_sequence
```

可以恢复 same-Stream AcceptanceSequence 顺序。

但：

> physical row order / rowid 仍然不是 Canonical ordering。

### Timestamp

```text
ClockReading int64
    → SQLite INTEGER
```

上述 DDL 已直接约束：

```text
present_mask bit = 0
    → tsN IS NULL

present_mask bit = 1
    → tsN IS NOT NULL
    → typeof(tsN) == 'integer'
```

因此 SQLite dynamic typing 不能把 `REAL/TEXT` 值悄悄混入 Canonical `int64 ClockReading`。

仍有一个跨表约束不能仅靠本表 `CHECK` 完成：

```text
timestamp_present_mask
必须不设置 referenced TimestampKeySet.key_count 之外的 bit
```

该约束必须在 Record Handoff/insert 前由 Plugin 验证，并由 offline Artifact validator 再检查。

因此：

```text
Absent
!= Present(0)
```

### Payload

```text
Payload
    → exact BLOB bytes
```

Empty Payload：

```text
zero-length BLOB
```

不是 NULL。

SQLite C API 有一个必须显式处理的边界：

```text
sqlite3_bind_blob(stmt, i, nullptr, 0, ...)
    → SQL NULL
```

因此 RoDAQ 合法的：

```text
size == 0
data == nullptr
```

不能直接按上述调用绑定。实现必须使用：

```text
sqlite3_bind_zeroblob(stmt, i, 0)
```

或等价的 non-NULL zero-length BLOB binding，确保 decode 后仍是 empty Payload 而不是 absent/NULL。

## 2.5 Descriptor Readiness

Descriptor graph 必须在 Binding Active 前 transactionally ready。

推荐：

```text
BEGIN
insert missing Schema / Clock / TKS
insert StreamDescriptor
validate constraints
COMMIT
        ↓
Descriptor Ready
```

不得把 Record INSERT 的 foreign-key failure 当作正常的 lazy descriptor creation 机制。

`PRAGMA foreign_keys=ON` 应作为 defensive integrity mechanism。

---

## 2.6 V1 SQLite3 Plugin 内部组织与配置

V1 建议采用一个 writer connection + 一组 prepared statements：

```text
sqlite3* db
artifact statement set
descriptor insert statements
record insert statement
transaction state
pending persistence completion batch
```

不需要 connection pool。

### one writer connection

原因：

```text
SQLite 写并发最终仍受 database writer serialization 约束
RoDAQ Runtime 已有 single logical Storage Dispatch Lane
```

强行引入多个 writer connection 会增加：

```text
SQLITE_BUSY
lock contention
commit ordering
shutdown complexity
```

却没有明显 V1 收益。

### prepared statements

Descriptor 与 Record insert 应在 Prepare/first-use 阶段建立 prepared statements，并在 Close 前明确 finalize。

这样：

```text
steady-state Record handoff
```

不需要重复 parse SQL。

所有 bind 后都必须按固定 cleanup discipline：

```text
step
reset
clear_bindings
```

否则不仅会泄漏 binding lifetime，还可能导致 Runtime Payload release boundary 错误。

### transaction batching policy

V1 应支持：

```text
max_records_per_transaction
max_bytes_per_transaction
max_transaction_age
```

任一达到阈值即可触发 COMMIT。

这样同时限制：

```text
persistence completion latency
rollback blast radius
transaction memory / WAL growth
```

具体默认值应由 benchmark 冻结，不在设计文档凭经验拍值。

### pragma profiles

建议至少定义两个 preset：

```text
balanced
resilient
```

`balanced`：

> 面向普通机器人本地高吞吐记录，但仍保持数据库结构正确。

`resilient`：

> 提高 crash/power-loss resistance，接受额外 fsync/latency。

具体 pragma 必须在实现阶段根据目标 SQLite 版本和 filesystem benchmark 验证。

不建议 V1 暴露任意用户 pragma 字符串作为唯一配置方式，因为这样很难对：

```text
PersistenceSucceeded
NormalComplete
```

给出稳定 guarantee。

可以允许 advanced override，但必须标记为可能改变 durability guarantee。

### index

V1 至少建议：

```text
PRIMARY KEY(stream_id, acceptance_sequence)
```

并根据实际查询需求再增加：

```text
StreamDescriptorId
record time projection
SourceId
```

二级索引。

不能为了“查询方便”一次创建大量 index，因为每条 Record insert 都要付写放大成本。

### Artifact validator

SQLite Plugin 应同时提供最小 offline validation path，至少检查：

```text
artifact state
PRAGMA integrity_check / quick_check policy
foreign_key_check
storage-class checks for BLOB-backed Canonical values
Canonical Identifier BLOBs decode as valid UTF-8 byte strings
Descriptor graph referential integrity
Record timestamp mask consistency
U64BE field length
```

这不要求 V1 做完整 reader/replay 产品，但能够验证 Storage full-fidelity contract。

---

## 2.7 Stable Plugin Lifecycle / Service Mapping

SQLite3 Plugin 通过 V1 Storage Family Protocol 接入 Runtime。

```text
create
    → 构造 Plugin-local state
    → 不打开 database

bind
    → require rodaq.runtime.common.v1
    → require rodaq.runtime.canonical.v1
    → require rodaq.runtime.storage.v1
    → require rodaq.storage.control.v1

configure
    → 通过 Runtime-owned ConfigView 读取 target、batching、busy policy、pragma profile、resource bounds
    → 不保存 ConfigView 到调用返回之后

open
    → sqlite3_open_v2
    → schema / metadata initialization
    → prepared statement creation
    → Incomplete Artifact state

submit
    → 对应 §3 Handoff / Payload Binding

flush
    → commit 当前 flush-set transaction/batch

close(close_request)
    → NORMAL: final transaction + finalization status + statement cleanup + sqlite3_close；只有全部 NormalComplete 条件满足时才写 NormalComplete
    → ACTIVATION_ABORT: rollback/cleanup + Incomplete/Aborted artifact semantics，禁止 NormalComplete
    → FAILURE: failure cleanup，禁止把失败 Session 伪装为 NormalComplete
    → return 后 no future payload/persistence callback

destroy
    → 只在未 Open 或 Closed/quiesced state 调用
```

`sqlite3*`、`sqlite3_stmt*` 和其他 SQLite native type：

> 只存在于 Plugin implementation 内，不进入 Stable ABI / Runtime Public API。

---

# 3. Handoff、Payload Lifetime 与 Persistence

## 3.1 Writer Model

V1 SQLite 实现建议：

> 一个 logical writer lane / writer connection。

即使 SQLite build 是 threadsafe，也没有必要为了“并行”引入多个 writer connection。

单 writer 更容易保证：

```text
same-Stream ordering
transaction batching
completion order
Flush / Close
```

未来可以增加 read-only analysis connection。

## 3.2 Payload Binding

Handoff entry 时 Storage 只有：

```text
call-scoped read-only PayloadView
```

SQLite future implementation 可以选择：

```text
A. SQLITE_STATIC direct bind
B. copy to bounded Storage-owned buffer
```

### SQLITE_STATIC

SQLite contract 要求 bound pointer 一直有效，直到：

```text
parameter rebound/cleared
or
statement finalized
```

因此 direct path：

```text
if Payload non-empty:
    bind Payload with SQLITE_STATIC
else:
    bind explicit zero-length BLOB
        ↓
sqlite3_step()
        ↓
terminal step result
        ↓
sqlite3_reset()
sqlite3_clear_bindings()
        ↓
SQLite no longer references Runtime bytes
        ↓
PayloadConsumed
```

如果 `sqlite3_step()` 是 retryable：

> 不得提前 PayloadConsumed。

### SQLITE_TRANSIENT

SQLite 会在 bind return 前复制 bytes。

语义上可用，但：

> 产生额外 backend-owned copy，必须计入 Storage memory budget。

### HandoffAccepted

只有当 Record 已进入 bounded SQLite Storage responsibility，并且 Plugin 可以保证后续 terminal handling 时，才：

```text
HandoffAccepted
```

对于最简单的 single-writer direct `SQLITE_STATIC` path，推荐明确采用：

```text
bind Runtime Payload
    ↓
step reaches terminal insertion result
    ↓
reset + clear bindings
    ↓
HandoffAccepted
    ↓
PayloadConsumed
    ↓
Handoff return
```

此时后续 transaction COMMIT 只影响 Persistence，不再需要 Runtime Payload。

该 direct path 的 `HandoffAccepted` 与 `PayloadConsumed` 可以在同一个 synchronous Handoff return 中发生，不建立跨 return 的 asynchronous Payload Access Lease。

如果未来实现选择在 `HandoffAccepted` 后把仍绑定 Runtime Payload 的 work 交给异步 writer，则必须显式建立 generic Payload Access Lease，并在 reset/clear/finalize 后才 `PayloadConsumed`。不得在 SQLite 仍可能读取 bound pointer 时提前结束 lease。

### 3.2.1 Stable Storage ABI Mapping

SQLite3 Handoff 通过 `rodaq_storage_plugin::submit()` 表达。

`rodaq_storage_submission` 必须完整消费当前 Stable ABI 的 Canonical Record 投影：

```text
AcceptanceSequence
optional SourceHandle / SourceId
optional SourceSequence
complete TimestampStateMap
Payload
```

低频 Canonical Descriptor Graph 通过 `rodaq.runtime.canonical.v1` 查询，包括：

```text
StreamId / StreamDescriptorId
optional SchemaId
SchemaFormat / optional TypeIdentifier / representation
TimestampKeySetId / TimestampKey content
ClockId / exact reduced rational resolution / ClockOrigin
```

SQLite projection 必须持久化 Canonical value；Runtime Handle 和 transport slot index 不能作为持久化 identity。

首个 V1 single-writer direct path 推荐：

```text
bind Runtime Payload
    ↓
sqlite3_step() reaches terminal INSERT result
    ↓
sqlite3_reset()
sqlite3_clear_bindings()
    ↓
SQLite no longer references Runtime Payload
    ↓
HandoffAccepted
```

对应 ABI：

```text
submit() function return == OK

out_result.payload_state
    = RODAQ_STORAGE_PAYLOAD_CONSUMED

out_result.persistence_state
    = RODAQ_STORAGE_PERSISTENCE_PENDING
```

因此首个实现：

```text
不发送 payload_consumed() callback
```

因为 Runtime Payload 已在 `submit()` return 前被 SQLite 完全解除引用。

事务 COMMIT 成功后，对 batch 中各 Pending operation exactly-once 调用：

```text
rodaq.runtime.storage.v1::persistence_completed(
    operation,
    RODAQ_STORAGE_PERSISTENCE_SUCCEEDED,
    NULL)
```

如果 transaction 被明确 rollback：

```text
PERSISTENCE_FAILED
```

如果 backend/process failure 后无法证明 transaction outcome：

```text
PERSISTENCE_UNKNOWN
```

如果 INSERT 在 HandoffAccepted 前就 terminal failure：

```text
submit() returns non-OK
no payload callback
no persistence callback
no Runtime Payload access after return
```

如果未来实现改为异步 writer，并在 `submit()` return 后仍保留 SQLite binding / Runtime PayloadView：

```text
payload_state = PAYLOAD_PENDING
```

且必须在 reset/clear/finalize 真正结束所有 Runtime Payload access 后 exactly-once 调用：

```text
payload_consumed(operation)
```

不能因为 row 已进入 private queue 就提前结束 Payload lease。

## 3.3 Transaction Batching

单条 INSERT：

```text
sqlite3_step() == SQLITE_DONE
```

如果仍在 open transaction：

> 不等于 PersistenceSucceeded。

Record 继续：

```text
PersistencePending
```

推荐 batch：

```text
BEGIN IMMEDIATE

insert R1
insert R2
...
insert RN

COMMIT
        ↓
PersistenceSucceeded R1..RN
```

`COMMIT` 因 lock contention 返回 `SQLITE_BUSY` 时 transaction 可以保持 active，并按 bounded busy policy retry；在 retry terminal outcome 之前相关 Record 保持：

```text
PersistencePending
```

不得把一次 retryable BUSY 立即报告为成功或丢弃 transaction。

更一般地，SQLite 在以下错误后：

```text
SQLITE_FULL
SQLITE_IOERR
SQLITE_NOMEM
SQLITE_BUSY / SQLITE_LOCKED family
```

可能只回滚当前 statement，也可能使整个 transaction 自动结束。Plugin 不得仅根据 error class 猜测 transaction 是否仍 active。

发生 transaction-affecting error 后必须检查：

```text
sqlite3_get_autocommit(db)
(and sqlite3_txn_state() when available / useful)
```

以确认：

```text
transaction still active
    → bounded retry or explicit ROLLBACK

transaction already rolled back
    → affected batch PersistenceFailed

COMMIT success observed
    → affected batch PersistenceSucceeded

backend/process outcome cannot be proven
    → PersistenceUnknown
```

不得在 COMMIT 前把各行提前报告 success。

## 3.4 Busy / Lock / Backpressure

SQLite：

```text
SQLITE_BUSY
SQLITE_LOCKED
I/O stall
```

是 backend condition。

不能因此无限积压 Runtime Record。

Storage 必须 bounded 地管理：

```text
busy retry / timeout
transaction size
queued Record count
queued Payload bytes
```

Stop/failure deadline 必须能中断这些等待。

在 busy/error interruption 后，Plugin 必须先确认 SQLite transaction state，再决定 retry / rollback / failure completion；不能把“等待被中断”本身等同于 transaction 已回滚。

不得为了继续写数据库而：

```text
drop old Record
silent skip INSERT
```

## 3.5 Effective Payload Limit

SQLite 有：

```text
SQLITE_LIMIT_LENGTH
build-time maximum length
```

Storage capability 必须计算：

```text
storage_payload_limit
<= actual SQLite supported BLOB length
```

并保留实现所需 margin。

Runtime 再建立：

```text
effective_payload_limit
=
min(runtime_payload_limit,
    storage_payload_limit)
```

所以 oversize：

> Gate-pass 前拒绝。

不得等到 INSERT 才正常发现超限。

---

# 4. Artifact State、Failure 与 Finalization

## 4.1 Artifact State

Artifact state 至少：

```text
Incomplete
NormalComplete
```

实现可以增加：

```text
Aborted
Failed
```

作为 diagnostic state。

只有：

```text
NormalComplete
```

具有规范性 normal-close 含义。

Prepare：

```text
state = Incomplete
```

Starting activation abort：

```text
rollback open transaction
settle descriptor work
close writer resource
state remains Incomplete / Aborted
```

connection close success：

> 不代表 Artifact 正常完成。

## 4.2 Flush

Final Flush：

```text
finish current transaction
obtain COMMIT / rollback terminal result
deliver required persistence completions
verify no required Record remains Pending
```

Flush timeout / unrecoverable SQLite failure：

```text
terminal disposition = Failure
```

Flush success：

> 不等于 power-loss durability，也不等于 NormalComplete。

## 4.3 Journal Mode / Durability

V1 Mapping 不冻结：

```text
journal_mode
synchronous
```

因为它们属于 deployment/performance/durability policy。

必须区分：

```text
SQLite COMMIT success
    → RoDAQ PersistenceSucceeded

power loss after COMMIT
    → depends on SQLite journal/synchronous/filesystem policy
```

例如 WAL + `synchronous=NORMAL`：

> 可以保持 consistency，但最近 transaction 在 power loss 后仍可能丢失。

所以：

```text
PersistenceSucceeded
!=
power-loss durability
```

RoDAQ V1 normal SQLite Artifact 冻结为：

> **一个可独立搬运、打开和验证的主 `.sqlite3` 数据库文件；`-wal` / `-shm` 不属于 NormalComplete Artifact。**

因此如果运行阶段使用 WAL，finalization 必须显式离开“主文件仍依赖 WAL sidecar”的状态。V1 推荐的逻辑序列为：

```text
final data transaction COMMIT
        ↓
stop all future Storage writes
        ↓
ensure no external writer / finalization-blocking reader
        ↓
PRAGMA wal_checkpoint(TRUNCATE)
        ↓
verify checkpoint completed without BUSY / I/O error
        ↓
PRAGMA journal_mode=DELETE
(or another configured rollback-journal mode)
        ↓
verify mode switch succeeded
        ↓
finalize all long-lived prepared statements
close all sqlite3_blob / backup handles
        ↓
BEGIN finalization transaction
UPDATE artifact SET state='NormalComplete'
COMMIT
(using transient exec/statement that is finalized before return)
        ↓
verify autocommit state restored
verify no outstanding statements / BLOB handles
        ↓
sqlite3_close()
```

关键点：

> **不能先在 WAL mode 写 `NormalComplete`，然后只关闭 connection。**

因为该 marker 自己可能仍只存在于 `-wal`，这与“主 `.sqlite3` 文件自包含”的 Artifact contract 冲突。

`wal_checkpoint(TRUNCATE)` 失败、返回 BUSY、journal-mode switch 未真正成功、或者 final marker transaction 失败：

> Artifact 继续按 Incomplete / Failed 处理，不能因为之前 data transaction 已 COMMIT 就宣称 NormalComplete。

`state='NormalComplete'` row 是 persistent finalization marker，但不是 online `CloseSuccess` 的唯一证据。Runtime 仍必须成功完成最终 resource checks 和 `sqlite3_close()`。

离线 validator 可以在进程结束后基于：

```text
NormalComplete marker
+
main database self-contained / no required WAL state
+
integrity / foreign-key / schema invariants
```

判断 Artifact bytes 是否结构上 NormalComplete；这与“上一进程是否曾报告 CloseSuccess”是两个不同问题。

如果 deployment 明确不允许在 finalization 时取得切换 journal mode 所需的排他条件：

> 该配置不满足当前 V1 单文件 NormalComplete Artifact contract，必须在 Prepare 阶段拒绝或选择非 WAL finalization-compatible policy。

当前证据仍不足以要求 generic `persistence.md`。

## 4.4 Normal Close

Normal Close：

```text
no future Record Handoff
        ↓
final data transaction COMMIT
        ↓
all persistence completions delivered
        ↓
if WAL:
    checkpoint(TRUNCATE)
    + verify success
    + switch to rollback-journal mode
    + verify success
        ↓
finalize long-lived prepared statements
close BLOB / backup handles
        ↓
write Artifact state = NormalComplete
in a transient final rollback-journal transaction
        ↓
verify finalization statement is finalized
verify autocommit / no outstanding handles
        ↓
sqlite3_close() == SQLITE_OK
```

如果仍有未 finalize statement/BLOB/backup handle，`sqlite3_close()` 可能返回 `SQLITE_BUSY`：

> 这是 Close failure。

V1 正常实现不应把这类错误留到 `sqlite3_close()` 才发现；finalization barrier 必须在写 NormalComplete marker 前先清理长期 handle，并在 marker transaction 后再次验证没有 outstanding SQLite object。

如果 `NormalComplete` marker transaction 已写入，但后续 backend close/finalization 失败：

> Runtime 仍报告 Close failure。

Offline validator 不能只看 marker，还必须确认：

```text
database opens consistently
journal/finalization invariant satisfied
```

## 4.5 Fatal Failure

Fatal SQLite error 后，Plugin 必须先根据 SQLite error + `sqlite3_get_autocommit()` / transaction state 判定当前 transaction 是否仍 active；不得默认所有 I/O/full/memory error 都已自动 rollback。

随后按能够证明的 backend state 分类：

```text
never HandoffAccepted
    → Runtime persistence Failed

HandoffAccepted
+ transaction proven rolled back
    → PersistenceFailed

HandoffAccepted
+ transaction committed
    → Succeeded remains Succeeded

commit/backend outcome uncertain
    → PersistenceUnknown
```

如果 direct `SQLITE_STATIC` binding 尚未解除：

> Runtime Payload 仍必须保持有效。

只有完成：

```text
statement/binding lifetime settlement
or
Storage failure quiescence
```

后才能 release。

Failure 不修改 Admission Accepted history。

---

# 5. Conformance 与设计审查结论

## 5.1 Common Canonical Corpus

SQLite3 与 MCAP 必须最终使用同一 Canonical Corpus。

至少覆盖：

### uint64

```text
Canonical ID = 0
Canonical ID near UINT64_MAX
AcceptanceSequence = 0 / high uint64
SourceSequence absent
SourceSequence present(0) with SourceId present
SourceSequence present while SourceId absent → rejected by schema/validator
high SourceSequence
resolution num/den > INT64_MAX where valid
```

### int64 ClockReading

```text
INT64_MIN
-1
0
INT64_MAX
```

### Presence

```text
SchemaId absent
SchemaId present(0)
SourceId absent
binary non-empty SourceId
Timestamp Absent
Timestamp Present(0)
```

### Opaque Bytes

```text
empty Payload stored as zero-length BLOB, never NULL
binary Payload with NUL / 0xff
binary Schema bytes
exact Canonical UTF-8 identifier bytes
```

### Evolution

```text
multiple StreamDescriptors for one Stream
multiple Clocks / TKS
multiple Streams
```

Decode：

> 必须恢复 same Canonical graph/state。

## 5.2 Ordering / Query

必须验证：

```text
ORDER BY stream_id, acceptance_sequence
using U64BE
    → unsigned sequence order

descriptor FK ready before Record

same-Stream insert/commit path
does not reorder Accepted Records
```

不得依赖：

```text
rowid
physical row order
```

## 5.3 Payload Lifetime

至少验证：

```text
SQLITE_STATIC bind
    ↓
step success
    ↓
reset + clear bindings
    ↓
release/mutate original Runtime bytes
    ↓
stored BLOB exact
```

并验证首个 direct path 的 Stable ABI result：

```text
submit() == OK
submit_result.payload_state == PAYLOAD_CONSUMED
submit_result.persistence_state == PERSISTENCE_PENDING
```

随后 transaction COMMIT：

```text
→ exactly one persistence_completed(SUCCEEDED)
```

不得再发送：

```text
payload_consumed()
```

因为 Payload axis 已在 submit result 中同步 terminal。

另外覆盖：

```text
retryable SQLITE_BUSY
→ no early PayloadConsumed

SQLITE_TRANSIENT
→ extra copied memory accounted
```

## 5.4 Failure / Finalization

注入：

```text
COMMIT SQLITE_BUSY with transaction still active
statement error with automatic transaction rollback
statement error with transaction still active
explicit rollback
SQLITE_BUSY timeout
BLOB length limit
unfinalized statement on close
WAL checkpoint BUSY / failure when configured
journal_mode switch failure
NormalComplete marker attempted while still WAL-dependent
Starting activation abort
fatal error with open transaction
```

分别验证：

```text
HandoffAccepted
PayloadConsumed
PersistenceSucceeded / Failed / Unknown
NormalComplete
```

## 5.5 设计审查结论

SQLite3 能完整表示当前 V1 Canonical State。

最重要的 backend-specific 结论：

> Profile full `uint64` domain 不能直接依赖 SQLite signed INTEGER。

使用：

```text
fixed 8-byte big-endian BLOB
```

可以同时解决：

```text
full uint64 range
exact round-trip
unsigned ordering
zero/presence ambiguity
```

SQLite 独立验证了：

```text
Descriptor-before-Record
transaction batch persistence
Absent vs Present(0)
PayloadConsumed vs PersistenceSucceeded
backend durability policy separation
explicit Artifact completion state
```

当前仍没有证据需要新增 generic `persistence.md`。

## 5.6 Implementation Status

SQLite3 Storage Mapping v0.5 是 future / designed target：

> 不参与 V1 implementation、interoperability qualification 或 release completion。

正式实现前需要重新核对：

```text
actual SQLite version
compile limits
journal mode
synchronous policy
batch size
busy timeout
```

这些 tuning 不得改变本文 Canonical mapping。

## 5.7 Native Evidence / Executable Validation

当前开发机使用 SQLite 3.37.2 library 对本文 DDL 做了实际验证。

验证内容：

```text
create all tables
foreign_keys = ON
Canonical ID = 0
AcceptanceSequence = 0
AcceptanceSequence = UINT64_MAX
SourceId binary bytes
SourceSequence = UINT64_MAX
ClockReading = 0 / -1
empty / binary Payload
ORDER BY U64BE
foreign_key_check
```

结果：

```text
SQLITE_MAPPING_DDL_ROUNDTRIP = PASS
```

此外对 V1 WAL finalization sequence 做了本地 SQLite 3.37.2 executable validation：

```text
journal_mode = WAL
write + COMMIT data
    → -wal / -shm exist

wal_checkpoint(TRUNCATE)
    → success

journal_mode = DELETE
    → success

final NormalComplete transaction
    → success

close
    → no -wal / -shm sidecar

reopen main .sqlite3
    → NormalComplete visible
    → payload exact
```

结果：

```text
SQLITE_WAL_FINALIZATION_CHECK = PASS
```

因此本文的主要 relational/scalar mapping 与单文件 WAL finalization path 均通过本地可执行 sanity check，而不是只停留在理论层面。
