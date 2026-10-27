import { useEffect, useMemo, useRef } from "react";
import uPlot from "uplot";
import "uplot/dist/uPlot.min.css";
import { mergeSeries, type Series } from "../lib/format";

type Props = { title: string; series: Series[]; unit?: string; max?: number };

const css = (name: string) =>
  getComputedStyle(document.documentElement).getPropertyValue(name).trim() || "#888";

const PALETTE = ["--c1", "--c2", "--c3", "--c4", "--c5", "--c6", "--c7", "--c8"];

export function Chart({ title, series, unit = "", max }: Props) {
  const host = useRef<HTMLDivElement>(null);
  const plot = useRef<uPlot | null>(null);
  const data = useMemo(() => mergeSeries(series), [series]);
  const labels = series.map((s) => s.label).join("\u0000");
  const single = series.length === 1;

  useEffect(() => {
    if (!host.current) return;
    const axis = { stroke: css("--muted"), grid: { stroke: css("--grid") }, ticks: { stroke: css("--grid") } };
    const opts: uPlot.Options = {
      width: host.current.clientWidth || 600,
      height: 180,
      legend: { show: !single },
      cursor: { drag: { x: false, y: false } },
      scales: { y: max !== undefined ? { range: [0, max] } : {} },
      axes: [axis, { ...axis, size: 70, values: (_u, v) => v.map((x) => `${x}${unit}`) }],
      series: [
        {},
        ...labels.split("\u0000").filter(Boolean).map((label, i) => ({
          label,
          spanGaps: true,
          stroke: css(single ? "--accent" : PALETTE[i % PALETTE.length]),
          width: 1.5,
          fill: single ? css("--accent-fill") : undefined,
        })),
      ],
    };
    plot.current = new uPlot(opts, data as uPlot.AlignedData, host.current);
    const ro = new ResizeObserver(() => {
      if (host.current && plot.current) plot.current.setSize({ width: host.current.clientWidth, height: 180 });
    });
    ro.observe(host.current);
    return () => {
      ro.disconnect();
      plot.current?.destroy();
      plot.current = null;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [unit, max, labels]);

  useEffect(() => {
    plot.current?.setData(data as uPlot.AlignedData);
  }, [data]);

  const empty = data[0].length === 0;
  return (
    <section className="panel">
      <h3>{title}</h3>
      <div ref={host} />
      {empty && <div className="empty">no data in this range</div>}
    </section>
  );
}
