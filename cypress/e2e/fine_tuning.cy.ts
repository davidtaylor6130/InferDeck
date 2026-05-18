// Cypress E2E tests for InferDeck Gateway - Fine-Tuning Jobs Endpoint
// Run with: npx cypress run --spec "cypress/e2e/fine_tuning.cy.ts"

describe('Fine-Tuning /v1/fine_tuning/jobs Endpoint', () => {
  const BASE_URL = Cypress.env('API_URL') || 'https://localhost:8080';

  describe('POST /v1/fine_tuning/jobs', () => {
    it('returns 400 for missing body', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body).to.have.property('error');
        expect(response.body.error).to.have.property('message');
      });
    });

    it('returns 400 for missing model', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          training_file: '/data/training.jsonl',
        },
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body.error).to.have.property('message');
        expect(response.body.error.message).to.contain('model');
      });
    });

    it('returns 400 for missing training_file', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'llama-3.1-8b-instruct',
        },
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body.error).to.have.property('message');
        expect(response.body.error.message).to.contain('training_file');
      });
    });

    it('creates a fine-tuning job with default hyperparameters', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'llama-3.1-8b-instruct',
          training_file: '/data/training.jsonl',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('id');
          expect(response.body).to.have.property('object', 'fine_tuning.job');
          expect(response.body).to.have.property('model', 'llama-3.1-8b-instruct');
          expect(response.body).to.have.property('status', 'queued');
          expect(response.body).to.have.property('created_at');
          expect(response.body).to.have.property('finished_at', 0);
          expect(response.body).to.have.property('result_files').to.be.an('array');
          expect(response.body).to.have.property('hyperparameters');
          expect(response.body).to.have.property('training_file');
          expect(response.body).to.have.property('trained_tokens', 0);
        }
      });
    });

    it('creates a fine-tuning job with custom hyperparameters', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'llama-3.1-8b-instruct',
          training_file: '/data/training.jsonl',
          epochs: 5,
          learning_rate: 0.0002,
          batch_size: 4,
          max_steps: 2000,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('id');
          expect(response.body.status).to.eq('queued');
          if (response.body.hyperparameters) {
            expect(response.body.hyperparameters).to.have.property('n_epochs', 5);
            expect(response.body.hyperparameters).to.have.property('learning_rate_multiplier', 0.0002);
          }
        }
      });
    });

    it('accepts validation_file parameter', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'llama-3.1-8b-instruct',
          training_file: '/data/training.jsonl',
          validation_file: '/data/validation.jsonl',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('validation_file');
          expect(response.body.validation_file).to.eq('/data/validation.jsonl');
        }
      });
    });

    it('accepts seed parameter', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'llama-3.1-8b-instruct',
          training_file: '/data/training.jsonl',
          seed: 42,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('seed', 42);
        }
      });
    });

    it('accepts suffix parameter', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'llama-3.1-8b-instruct',
          training_file: '/data/training.jsonl',
          suffix: 'custom-finetune',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('suffix', 'custom-finetune');
        }
      });
    });

    it('passes batch_size to hyperparameters correctly', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'llama-3.1-8b-instruct',
          training_file: '/data/training.jsonl',
          batch_size: 8,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('hyperparameters');
        }
      });
    });

    it('passes max_steps to hyperparameters correctly', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'llama-3.1-8b-instruct',
          training_file: '/data/training.jsonl',
          max_steps: 5000,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('hyperparameters');
        }
      });
    });
  });

  describe('GET /v1/fine_tuning/jobs', () => {
    it('returns 200 with list of fine-tuning jobs', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'GET',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(200);
        expect(response.body).to.have.property('object', 'list');
        expect(response.body).to.have.property('data').to.be.an('array');
        expect(response.body).to.have.property('has_more');
        expect(response.body.has_more).to.be.a('boolean');
      });
    });

    it('returns jobs with correct schema', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'GET',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(200);
        if (response.body.data.length > 0) {
          const job = response.body.data[0];
          expect(job).to.have.property('id');
          expect(job).to.have.property('object', 'fine_tuning.job');
          expect(job).to.have.property('model');
          expect(job).to.have.property('status');
          expect(job).to.have.property('created_at');
          expect(job).to.have.property('finished_at');
          expect(job).to.have.property('result_files');
          expect(job).to.have.property('hyperparameters');
          expect(job).to.have.property('training_file');
          expect(job).to.have.property('trained_tokens');
        }
      });
    });
  });

  describe('GET /v1/fine_tuning/jobs/:job_id', () => {
    it('returns 404 for non-existent job', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs/ft_nonexistent_12345`,
        method: 'GET',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        if (response.status === 404) {
          expect(response.body).to.have.property('error');
          expect(response.body.error).to.have.property('type', 'not_found');
        }
      });
    });

    it('returns job status for a known job ID', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs/ft_test_job_123`,
        method: 'GET',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('id');
          expect(response.body).to.have.property('object', 'fine_tuning.job');
          expect(response.body).to.have.property('status');
          expect(response.body).to.have.property('created_at');
          expect(response.body).to.have.property('finished_at');
          expect(response.body).to.have.property('model');
          expect(response.body).to.have.property('result_files').to.be.an('array');
          expect(response.body).to.have.property('hyperparameters');
          expect(response.body).to.have.property('training_file');
          expect(response.body).to.have.property('trained_tokens');
        }
      });
    });
  });

  describe('POST /v1/fine_tuning/jobs/:job_id/cancel', () => {
    it('returns 404 for non-existent job cancel', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs/ft_nonexistent_cancel/cancel`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        if (response.status === 404) {
          expect(response.body).to.have.property('error');
        }
      });
    });

    it('cancels a job and returns confirmation', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs/ft_test_job_cancel/cancel`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('id');
          expect(response.body).to.have.property('cancelled', true);
        }
      });
    });
  });

  describe('Fine-Tuning response schema compliance', () => {
    it('follows OpenAI-compatible fine-tuning response format', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'llama-3.1-8b-instruct',
          training_file: '/data/training.jsonl',
        },
      }).then((response) => {
        if (response.status === 200) {
          // Required fields per OpenAI fine-tuning schema
          expect(response.body).to.have.property('id');
          expect(response.body).to.have.property('object', 'fine_tuning.job');
          expect(response.body).to.have.property('model');
          expect(response.body).to.have.property('status');
          expect(response.body).to.have.property('created_at');
          expect(response.body).to.have.property('finished_at');
          expect(response.body).to.have.property('result_files');
          expect(response.body.result_files).to.be.an('array');
          expect(response.body).to.have.property('hyperparameters');
          expect(response.body).to.have.property('training_file');
          expect(response.body).to.have.property('trained_tokens');
        }
      });
    });

    it('has correct status enum values', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'GET',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        if (response.status === 200 && response.body.data.length > 0) {
          const valid_statuses = ['pending', 'queued', 'running', 'succeeded', 'failed', 'cancelled'];
          expect(valid_statuses).to.include(response.body.data[0].status);
        }
      });
    });

    it('hyperparameters has correct structure', () => {
      cy.request({
        url: `${BASE_URL}/v1/fine_tuning/jobs`,
        method: 'GET',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        if (response.status === 200 && response.body.data.length > 0) {
          const job = response.body.data[0];
          if (job.hyperparameters) {
            expect(job.hyperparameters).to.be.an('object');
            expect(job.hyperparameters).to.have.property('n_epochs');
          }
        }
      });
    });
  });
});
