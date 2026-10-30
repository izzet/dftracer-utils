"""Agent dimensions and token metrics survive an indexed typed read."""

import gzip
import json

import pyarrow as pa
import pytest

from dftracer.utils import AggregationConfig, Indexer, TraceViewer


def agent_trace(tmp_path, steps, group_keys=("step", "workflow_id")):
    path = tmp_path / "agent.pfw.gz"
    events = []
    for step in steps:
        # Two calls in the same bucket must merge within a step, but never
        # across steps; zero is a real step, and tokens may be absent.
        for tokens in (120 + 10 * int(step), None):
            args = {"step": step, "workflow_id": "toy"}
            if tokens is not None:
                args["completion_tokens"] = tokens
            events.append(
                dict(name="call", cat="llm", pid=1, tid=1, ts=100, dur=10, ph="X", args=args)
            )
    path.write_bytes(gzip.compress(("\n".join(map(json.dumps, events)) + "\n").encode()))
    with Indexer(
        files=[str(path)],
        index_dir=str(tmp_path / "index"),
        require_aggregation=AggregationConfig(
            group_keys=list(group_keys),
            custom_metric_fields=["completion_tokens", "absent_tokens"],
        ),
    ) as indexer:
        status = indexer.ensure_indexed()
    return TraceViewer([str(path)], index_path=status.index_path)


@pytest.mark.parametrize("steps", [list(range(7)), list(map(str, range(7)))])
@pytest.mark.parametrize(
    "key,column", [("args.step", "args.step"), ("arg:step", "step"), ("step", "step")]
)
def test_typed_extra_dimensions(tmp_path, steps, key, column):
    tv = agent_trace(tmp_path, steps)
    regular = pa.table(
        tv.group_by("cat", "name", key, "args.workflow_id")
        .time_bucket(5_000_000)
        .agg("count", "sum:dur")
        .collect_typed()["regular"]
    ).to_pydict()
    assert sorted(regular[column]) == list(map(str, range(7)))
    assert regular["args.workflow_id"] == ["toy"] * 7
    assert regular["count"] == [2] * 7
    assert regular["sum_dur"] == [20] * 7


@pytest.mark.parametrize("field", ["completion_tokens", "args.completion_tokens"])
def test_typed_custom_token_metrics(tmp_path, field):
    tv = agent_trace(tmp_path, list(map(str, range(7))))
    regular = pa.table(
        tv.group_by("cat", "name", "args.step")
        .agg("count", f"sum:{field}", f"min:{field}", f"max:{field}", f"mean:{field}")
        .collect_typed()["regular"]
    ).to_pydict()
    assert "count" in regular, "typed read fell back to unaggregated raw rows"
    rows = sorted(zip(regular["args.step"], regular["sum_" + field]))
    assert rows == [(str(i), 120 + 10 * i) for i in range(7)]
    for op in ("min", "max", "mean"):
        assert regular[op + "_" + field] == regular["sum_" + field]


def test_typed_scalar_keys_preserve_precision(tmp_path):
    steps = [-1, 0, 1.5, 2**53 + 1]
    tv = agent_trace(tmp_path, steps)
    regular = pa.table(tv.group_by("arg:step").agg("count").collect_typed()["regular"]).to_pydict()
    assert sorted(regular["step"]) == sorted(map(str, steps))
    assert regular["count"] == [2] * len(steps)


def test_unconfigured_dimension_declines_tier(tmp_path):
    tv = agent_trace(tmp_path, [0, 1])
    # pid is retained as an envelope dimension, but an arbitrary args key
    # was not. The tier must decline rather than fabricate an empty group.
    regular = pa.table(tv.group_by("args.not_retained").agg("count").collect_typed()["regular"])
    assert "count" not in regular.column_names
    assert regular.num_rows == 4


def test_typed_custom_metric_without_extra_dimensions(tmp_path):
    tv = agent_trace(tmp_path, range(7), group_keys=())
    regular = pa.table(
        tv.group_by("cat")
        .agg("count", "sum:completion_tokens", "mean:completion_tokens")
        .collect_typed()["regular"]
    ).to_pylist()
    assert regular == [
        {"cat": "llm", "count": 14, "sum_completion_tokens": 1050, "mean_completion_tokens": 150}
    ]


def test_typed_configured_but_absent_metric(tmp_path):
    tv = agent_trace(tmp_path, [0])
    regular = pa.table(
        tv.group_by("cat")
        .agg("count", "sum:absent_tokens", "mean:absent_tokens")
        .collect_typed()["regular"]
    ).to_pylist()
    assert regular == [
        {"cat": "llm", "count": 2, "sum_absent_tokens": 0, "mean_absent_tokens": None}
    ]


def test_dfanalyzer_ipc_retains_declared_dimensions_and_metrics(tmp_path):
    from dftracer.utils.dfanalyzer import (
        _ipc_to_pandas,
        _typed_read_to_ipc,
        typed_group_keys,
        view_typed_frames,
    )

    agent_trace(tmp_path, range(7))
    # The task entry point used by both NullClient and real Dask workers.
    ipc = _typed_read_to_ipc(
        [str(tmp_path / "agent.pfw.gz")],
        str(tmp_path / "index" / ".dftindex"),
        5.0,
        1e6,
        group_keys=(*typed_group_keys(), "arg:step", "arg:workflow_id"),
        metric_fields=("completion_tokens",),
    )
    events = _ipc_to_pandas(ipc["events"]).sort_values("step")
    assert list(events["step"]) == list(map(str, range(7)))
    assert list(events["workflow_id"]) == ["toy"] * 7
    assert list(events["completion_tokens"]) == [120 + 10 * i for i in range(7)]
    assert list(events["count"]) == [2] * 7
    assert ipc["profiles"] is None
    frames = view_typed_frames(
        [str(tmp_path / "agent.pfw.gz")],
        str(tmp_path / "index" / ".dftindex"),
        time_granularity=5.0,
        group_keys=(*typed_group_keys(), "arg:step", "arg:workflow_id"),
        metric_fields=("completion_tokens",),
    )
    direct = frames["events"].sort_values("step")
    assert list(direct["step"]) == list(events["step"])
    assert list(direct["completion_tokens"]) == list(events["completion_tokens"])


def test_preaggregated_profile_preserves_numeric_keys_and_custom_stats(tmp_path):
    path = tmp_path / "profile.pfw.gz"
    event = dict(
        ph="C",
        cat="llm",
        name="call",
        pid=1,
        tid=1,
        ts=100,
        args={
            "step": 0,
            "count": 3,
            "dur_sum": 90,
            "dur_min": 20,
            "dur_max": 40,
            "completion_tokens_sum": 60,
            "completion_tokens_min": 10,
            "completion_tokens_max": 30,
        },
    )
    path.write_bytes(gzip.compress((json.dumps(event) + "\n").encode()))
    with Indexer(
        files=[str(path)],
        index_dir=str(tmp_path / "index"),
        require_aggregation=AggregationConfig(
            group_keys=["step"], custom_metric_fields=["completion_tokens"]
        ),
    ) as indexer:
        status = indexer.ensure_indexed()
    typed = (
        TraceViewer([str(path)], index_path=status.index_path)
        .group_by("cat", "arg:step")
        .agg(
            "count",
            "sum:dur",
            "sum:completion_tokens",
            "min:completion_tokens",
            "max:completion_tokens",
            "mean:completion_tokens",
        )
        .collect_typed()
    )
    assert pa.table(typed["regular"]).num_rows == 0
    assert pa.table(typed["aggregated"]).to_pylist() == [
        {
            "cat": "llm",
            "step": "0",
            "count": 3,
            "sum_dur": 90,
            "sum_completion_tokens": 60,
            "min_completion_tokens": 10,
            "max_completion_tokens": 30,
            "mean_completion_tokens": 20,
        }
    ]
