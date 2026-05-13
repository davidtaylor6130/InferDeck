import React from 'react';
import type { PageProps, ServiceRecord } from '../types';
import { ServiceStatusCard } from '../components/ServiceStatusCard';
import { SectionCard } from '../components/SectionCard';
import { DASHBOARD_URL, OLLAMA_BACKEND } from '../utils';

export const ServicesPage: React.FC<PageProps> = ({ state, actions }) => {
  const services = normalizeServices(state.servicesList);
  return (
    <SectionCard title="Service Control" eyebrow="Gateway, Ollama, and worker health">
      <div className="grid gap-4 md:grid-cols-2 2xl:grid-cols-3">
        {services.map(service => <ServiceStatusCard key={service.id} service={service} onStart={actions.startService} onStop={actions.stopService} onRestart={(id) => id === 'ollama' ? actions.restartOllama() : actions.restartService(id)} onCopied={actions.toast} />)}
      </div>
    </SectionCard>
  );
};

function normalizeServices(services: ServiceRecord[]): ServiceRecord[] {
  const fallback: ServiceRecord[] = [
    { id: 'gateway', name: 'Gateway', kind: 'gateway', status: 'running', pid: null, baseUrl: DASHBOARD_URL, lastHealthcheckAt: new Date().toISOString() },
    { id: 'ollama', name: 'Ollama', kind: 'ollama', status: 'running', pid: null, baseUrl: OLLAMA_BACKEND, lastHealthcheckAt: new Date().toISOString() },
    { id: 'comfyui', name: 'ComfyUI', kind: 'comfyui', status: 'not_configured', baseUrl: 'http://127.0.0.1:8188', lastError: 'Configure command and enable this service in gateway.local.yaml' },
    { id: 'rag-worker', name: 'RAG Worker', kind: 'rag-worker', status: 'not_configured', lastError: 'Configure command and enable this service in gateway.local.yaml' },
    { id: 'speech-worker', name: 'Speech Worker', kind: 'speech-worker', status: 'not_configured', lastError: 'Configure command and enable this service in gateway.local.yaml' },
  ];
  return fallback.map(item => services.find(s => s.kind === item.kind || s.id === item.id) || item);
}
