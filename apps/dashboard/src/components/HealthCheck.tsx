/**
 * Health Check Component
 * Displays the current status of the gateway service
 */

import React from 'react';

interface HealthCheckProps {
  data: {
    status: string;
    uptime: number;
    system?: {
      gpu: {
        available: boolean;
        memory: { total: number; used: number; free: number };
        temperature?: number;
      }[];
      cpu: { load: number };
      memory: { total: number; used: number; free: number };
    };
    database?: {
      status: string;
      size: number;
      sizeOnDisk: number;
      lastCheckpoint: string;
    };
    services?: {
      ollama?: {
        status: string;
        version: string;
        installed_models: number;
        loaded_models: number;
      };
    };
  };
}

const HealthCheck: React.FC<HealthCheckProps> = ({ data }) => {
  const getStatusColor = (status: string): string => {
    switch (status) {
      case 'healthy':
        return 'text-green-500';
      case 'healthy but degraded':
        return 'text-yellow-500';
      case 'unhealthy':
      case 'down':
        return 'text-red-500';
      default:
        return 'text-gray-400';
    }
  };

  const getHealthIcon = (status: string): string => {
    switch (status) {
      case 'healthy':
        return '✓';
      case 'healthy but degraded':
        return '⚠';
      case 'unhealthy':
      case 'down':
        return '✗';
      default:
        return '?';
    }
  };

  const formatBytes = (bytes: number): string => {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
  };

  const getMemoryColor = (used: number, total: number): string => {
    const percentage = (used / total) * 100;
    if (percentage < 70) return 'bg-green-500';
    if (percentage < 90) return 'bg-yellow-500';
    return 'bg-red-500';
  };

  const getHealthLabel = (status: string): string => {
    switch (status) {
      case 'healthy':
        return 'Healthy';
      case 'healthy but degraded':
        return 'Healthy (degraded)';
      case 'unhealthy':
        return 'Unhealthy';
      case 'down':
        return 'Down';
      default:
        return 'Unknown';
    }
  };

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between bg-white dark:bg-gray-800 p-4 rounded-lg shadow">
        <span className="text-gray-700 dark:text-gray-300 font-medium">Status</span>
        <span className={`text-lg font-semibold ${getStatusColor(data.status)}`}>
          {getHealthIcon(data.status)} {getHealthLabel(data.status)}
        </span>
      </div>

      <div className="bg-white dark:bg-gray-800 p-4 rounded-lg shadow">
        <h3 className="text-gray-700 dark:text-gray-300 font-medium mb-2">Uptime</h3>
        <p className="text-gray-700 dark:text-gray-300 font-semibold text-lg">
          {new Date(data.uptime).toLocaleString()}
        </p>
      </div>

      {data.system && (
        <div className="bg-white dark:bg-gray-800 p-4 rounded-lg shadow space-y-3">
          <h3 className="text-gray-700 dark:text-gray-300 font-medium">System Metrics</h3>

          {data.system.gpu && data.system.gpu.length > 0 && (
            <div>
              <p className="text-sm text-gray-500 dark:text-gray-400 mb-1">GPU</p>
              <div className="space-y-2">
                {data.system.gpu.map((gpu, index) => (
                  <div key={index} className="space-y-1">
                    <div className="flex justify-between text-sm">
                      <span className="text-gray-700 dark:text-gray-300">Memory</span>
                      <span className="text-gray-700 dark:text-gray-300">
                        {formatBytes(gpu.memory.used)} / {formatBytes(gpu.memory.total)}
                      </span>
                    </div>
                    <div className="w-full bg-gray-200 dark:bg-gray-700 rounded-full h-2">
                      <div
                        className={`h-2 rounded-full ${getMemoryColor(gpu.memory.used, gpu.memory.total)}`}
                        style={{ width: `${(gpu.memory.used / gpu.memory.total) * 100}%` }}
                      ></div>
                    </div>
                    {gpu.temperature && (
                      <div className="flex justify-between text-sm text-gray-500 dark:text-gray-400">
                        <span>Temperature</span>
                        <span>{gpu.temperature}°C</span>
                      </div>
                    )}
                  </div>
                ))}
              </div>
            </div>
          )}

          <div className="space-y-2">
            <div className="flex justify-between text-sm">
              <span className="text-gray-500 dark:text-gray-400">CPU Load</span>
              <span className="text-gray-700 dark:text-gray-300">{data.system.cpu.load}%</span>
            </div>
            <div className="w-full bg-gray-200 dark:bg-gray-700 rounded-full h-2">
              <div
                className={`h-2 rounded-full ${getMemoryColor(data.system.cpu.load, 100)}`}
                style={{ width: `${data.system.cpu.load}%` }}
              ></div>
            </div>
          </div>

          <div className="space-y-2">
            <div className="flex justify-between text-sm">
              <span className="text-gray-500 dark:text-gray-400">Memory</span>
              <span className="text-gray-700 dark:text-gray-300">
                {formatBytes(data.system.memory.used)} / {formatBytes(data.system.memory.total)}
              </span>
            </div>
            <div className="w-full bg-gray-200 dark:bg-gray-700 rounded-full h-2">
              <div
                className={`h-2 rounded-full ${getMemoryColor(data.system.memory.used, data.system.memory.total)}`}
                style={{ width: `${(data.system.memory.used / data.system.memory.total) * 100}%` }}
              ></div>
            </div>
          </div>
        </div>
      )}

      {data.database && (
        <div className="bg-white dark:bg-gray-800 p-4 rounded-lg shadow space-y-2">
          <h3 className="text-gray-700 dark:text-gray-300 font-medium">Database</h3>
          <div className="flex justify-between text-sm">
            <span className="text-gray-500 dark:text-gray-400">Status</span>
            <span className="text-gray-700 dark:text-gray-300">{data.database.status}</span>
          </div>
          <div className="flex justify-between text-sm">
            <span className="text-gray-500 dark:text-gray-400">Size</span>
            <span className="text-gray-700 dark:text-gray-300">
              {formatBytes(data.database.size)}
            </span>
          </div>
          <div className="flex justify-between text-sm">
            <span className="text-gray-500 dark:text-gray-400">Size on Disk</span>
            <span className="text-gray-700 dark:text-gray-300">
              {formatBytes(data.database.sizeOnDisk)}
            </span>
          </div>
          <div className="flex justify-between text-sm">
            <span className="text-gray-500 dark:text-gray-400">Last Checkpoint</span>
            <span className="text-gray-700 dark:text-gray-300">{data.database.lastCheckpoint}</span>
          </div>
        </div>
      )}

      {data.services && data.services.ollama && (
        <div className="bg-white dark:bg-gray-800 p-4 rounded-lg shadow space-y-2">
          <h3 className="text-gray-700 dark:text-gray-300 font-medium">Ollama Backend</h3>
          <div className="flex justify-between text-sm">
            <span className="text-gray-500 dark:text-gray-400">Status</span>
            <span className={`font-semibold ${getStatusColor(data.services.ollama.status)}`}>
              {getHealthIcon(data.services.ollama.status)} {getHealthLabel(data.services.ollama.status)}
            </span>
          </div>
          <div className="flex justify-between text-sm">
            <span className="text-gray-500 dark:text-gray-400">Version</span>
            <span className="text-gray-700 dark:text-gray-300">{data.services.ollama.version}</span>
          </div>
          <div className="flex justify-between text-sm">
            <span className="text-gray-500 dark:text-gray-400">Installed Models</span>
            <span className="text-gray-700 dark:text-gray-300">{data.services.ollama.installed_models}</span>
          </div>
          <div className="flex justify-between text-sm">
            <span className="text-gray-500 dark:text-gray-400">Loaded Models</span>
            <span className="text-gray-700 dark:text-gray-300">{data.services.ollama.loaded_models}</span>
          </div>
        </div>
      )}
    </div>
  );
};

export default HealthCheck;
