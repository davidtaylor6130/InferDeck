import React from 'react';
import type { PageProps } from '../types';
import { EmptyState } from '../components/EmptyState';
import { HardwareMetricCard } from '../components/HardwareMetricCard';
import { ResourceLockCard } from '../components/ResourceLockCard';
import { SectionCard } from '../components/SectionCard';
import { getQueueCounts } from '../utils';

export const HardwarePage: React.FC<PageProps> = ({ state }) => {
  const queue = getQueueCounts(state.statusData, state.jobsList);
  const telemetry = state.statusData?.hardware || state.statusData?.telemetry;
  const gpu = telemetry?.gpu;
  const vram = gpu?.memoryUsed != null && gpu?.memoryTotal != null ? `${(gpu.memoryUsed / 1024 / 1024 / 1024).toFixed(1)} / ${(gpu.memoryTotal / 1024 / 1024 / 1024).toFixed(1)} GB` : 'N/A';

  return (
    <div className="space-y-5">
      {(!telemetry || telemetry.available === false) && <EmptyState title="Hardware telemetry not connected yet. Enable AMD ADLX integration to view live GPU metrics." description={telemetry?.reason ? `Provider reported: ${telemetry.reason}` : 'The dashboard is waiting for the hardware provider.'} />}
      <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-4">
        <HardwareMetricCard label="GPU utilization" value={gpu?.utilization != null ? `${gpu.utilization}%` : 'N/A'} percent={gpu?.utilization ?? 0} tone="violet" />
        <HardwareMetricCard label="VRAM used/free" value={vram} percent={gpu?.memoryPercent ?? 0} tone="mint" />
        <HardwareMetricCard label="Temperature" value={gpu?.temperature != null ? `${gpu.temperature}°C` : 'N/A'} detail={gpu?.name || 'Radeon AI PRO R9700'} percent={gpu?.temperature ?? 0} tone="amber" />
        <HardwareMetricCard label="Power" value={gpu?.power != null ? `${gpu.power} W` : 'N/A'} detail="Board power" percent={gpu?.power ? Math.min(gpu.power / 3, 100) : 0} tone="cyan" />
        <HardwareMetricCard label="Fan speed" value={gpu?.fanSpeed != null ? `${gpu.fanSpeed}%` : 'N/A'} percent={gpu?.fanSpeed ?? 0} tone="cyan" />
        <HardwareMetricCard label="CPU usage" value={telemetry?.cpu?.utilization != null ? `${telemetry.cpu.utilization}%` : 'N/A'} percent={telemetry?.cpu?.utilization ?? 0} tone="violet" />
        <HardwareMetricCard label="RAM usage" value={telemetry?.memory?.percentage != null ? `${telemetry.memory.percentage}%` : 'N/A'} percent={telemetry?.memory?.percentage ?? 0} tone="mint" />
        <HardwareMetricCard label="Disk free" value={telemetry?.disk?.free != null ? `${telemetry.disk.free}` : 'N/A'} detail="Data volume" percent={telemetry?.disk?.percentage ?? 0} tone="mint" />
      </div>
      <SectionCard title="Resource Locks">
        <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-3">
          <ResourceLockCard locked={queue.gpuLocked} owner={queue.lockOwner} />
          <HardwareMetricCard label="LLM resource" value="gpu_llm" detail={queue.gpuLocked ? 'Allocated' : 'Available'} percent={queue.gpuLocked ? 100 : 0} tone={queue.gpuLocked ? 'amber' : 'mint'} />
          <HardwareMetricCard label="Image resource" value="gpu_image" detail="Available" percent={0} tone="mint" />
        </div>
      </SectionCard>
    </div>
  );
};
