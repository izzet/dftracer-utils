"""SST rebuilds must replace an aggregation tier, including its dictionary."""

import gzip
import json

import pyarrow as pa
import pytest

from dftracer.utils import AggregationConfig, Indexer, TraceViewer
from dftracer.utils.dask import distributed_index


def write_trace(path, count=4):
    rows = [
        dict(
            ph="X",
            cat="llm" if i % 2 == 0 else "POSIX",
            name="call" if i % 2 == 0 else "read",
            pid=1,
            tid=1,
            ts=1_000_000 + i * 1000,
            dur=100,
            args={"step": i // 2, "workflow_id": "demo", "completion_tokens": 10},
        )
        for i in range(count)
    ]
    path.write_bytes(gzip.compress(("\n".join(map(json.dumps, rows)) + "\n").encode()))


def totals(files, index_path):
    typed = (
        TraceViewer(files, index_path=str(index_path)).group_by("cat").agg("count").collect_typed()
    )
    return {r["cat"]: r["count"] for r in pa.table(typed["regular"]).to_pylist()}


@pytest.mark.parametrize("serial_first", [False, True])
def test_changed_config_and_force_rebuild_do_not_accumulate_rows(tmp_path, serial_first):
    path = tmp_path / "events.pfw.gz"
    write_trace(path)
    files = [str(path)]
    index_path = tmp_path / "index" / ".dftindex"
    configs = [
        AggregationConfig(time_interval_ms=1000),
        AggregationConfig(
            time_interval_ms=1000,
            group_keys=["step", "workflow_id"],
            custom_metric_fields=["completion_tokens"],
        ),
        AggregationConfig(
            time_interval_ms=10, group_keys=["step"], custom_metric_fields=["completion_tokens"]
        ),
        AggregationConfig(time_interval_ms=1000),
    ]
    if serial_first:
        with Indexer(files=files, index_dir=str(index_path), require_aggregation=configs[0]) as ix:
            ix.ensure_indexed()
    for config in configs:
        args = dict(
            files=files,
            index_path=str(index_path),
            local_staging=str(tmp_path / "stage"),
            shared_staging=str(tmp_path / "stage"),
            aggregation_config=config,
        )
        distributed_index(**args)
        assert totals(files, index_path) == {"llm": 2, "posix": 2}
        warm = distributed_index(**args)
        assert warm["artifact_batches"] == 0
    distributed_index(**args, force_rebuild=True)
    assert totals(files, index_path) == {"llm": 2, "posix": 2}


def test_reconfiguration_with_real_workers(tmp_path):
    from distributed import Client, LocalCluster

    paths = [tmp_path / f"events{i}.pfw.gz" for i in range(2)]
    for p in paths:
        write_trace(p)
    files = list(map(str, paths))
    index_path = tmp_path / "index" / ".dftindex"
    with LocalCluster(
        n_workers=2, threads_per_worker=1, processes=True, dashboard_address=None
    ) as cluster, Client(cluster) as client:
        for config in (
            AggregationConfig(),
            AggregationConfig(group_keys=["step"], custom_metric_fields=["completion_tokens"]),
            AggregationConfig(time_interval_ms=10),
        ):
            distributed_index(
                files=files,
                index_path=str(index_path),
                local_staging=str(tmp_path / "stage"),
                client=client,
                aggregation_config=config,
            )
            assert totals(files, index_path) == {"llm": 4, "posix": 4}


def test_changed_file_replaces_old_rows_from_every_file(tmp_path):
    paths = [tmp_path / f"events{i}.pfw.gz" for i in range(2)]
    for p in paths:
        write_trace(p)
    files = list(map(str, paths))
    args = dict(
        files=files,
        index_path=str(tmp_path / "index" / ".dftindex"),
        local_staging=str(tmp_path / "stage"),
        aggregation_config=AggregationConfig(),
    )
    distributed_index(**args)
    assert totals(files, args["index_path"]) == {"llm": 4, "posix": 4}
    write_trace(paths[0], 6)
    distributed_index(**args)
    assert totals(files, args["index_path"]) == {"llm": 5, "posix": 5}


@pytest.mark.parametrize("serial_first", [False, True])
def test_appending_a_file_preserves_existing_rows_and_labels(tmp_path, serial_first):
    first = tmp_path / "first.pfw.gz"
    write_trace(first)
    files = [str(first)]
    index_path = str(tmp_path / "index" / ".dftindex")
    config = AggregationConfig()
    args = dict(
        files=files,
        index_path=index_path,
        local_staging=str(tmp_path / "stage"),
        aggregation_config=config,
    )
    if serial_first:
        with Indexer(files=files, index_dir=index_path, require_aggregation=config) as ix:
            ix.ensure_indexed()
    else:
        distributed_index(**args)
    assert totals(files, index_path) == {"llm": 2, "posix": 2}
    added = tmp_path / "added.pfw.gz"
    write_trace(added, 2)
    files.append(str(added))
    result = distributed_index(**args)
    assert sum(result["per_worker"]) == 1
    assert totals(files, index_path) == {"llm": 3, "posix": 3}
