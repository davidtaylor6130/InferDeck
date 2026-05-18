// Cypress E2E tests for InferDeck C++ Gateway
// Run with: npx cypress run --spec "cypress/e2e/**/*.cy.ts"

describe('InferDeck Gateway E2E', () => {
  const BASE_URL = Cypress.env('API_URL') || 'https://localhost:8080';

  describe('/v1/health', () => {
    it('returns 200 with correct schema', () => {
      cy.request({
        url: `${BASE_URL}/v1/health`,
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(200);
        expect(response.body).to.have.property('status');
        expect(response.body).to.have.property('version');
        expect(response.body).to.have.property('uptime_seconds');
        expect(response.body).to.have.property('model_loaded');
        expect(response.body).to.have.property('gpu_available');
      });
    });
  });

  describe('/v1/models', () => {
    it('returns 200 with OpenAI-compatible schema', () => {
      cy.request({
        url: `${BASE_URL}/v1/models`,
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(200);
        expect(response.body).to.have.property('object', 'list');
        expect(response.body).to.have.property('data').to.be.an('array');
      });
    });
  });

  describe('/inferdeck/metrics', () => {
    it('returns 200 with counter/gauge/histogram structure', () => {
      cy.request({
        url: `${BASE_URL}/inferdeck/metrics`,
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(200);
        expect(response.body).to.have.property('counters').to.be.an('object');
        expect(response.body).to.have.property('gauges').to.be.an('object');
        expect(response.body).to.have.property('histograms').to.be.an('object');
      });
    });
  });

  describe('/inferdeck/status', () => {
    it('returns 200 with engine status', () => {
      cy.request({
        url: `${BASE_URL}/inferdeck/status`,
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(200);
        expect(response.body).to.have.property('initialized');
      });
    });
  });

  describe('/v1/chat/completions', () => {
    it('returns 400 for missing body', () => {
      cy.request({
        url: `${BASE_URL}/v1/chat/completions`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {},
      }).then((response) => {
        expect(response.status).to.be.oneOf([400, 415]);
        expect(response.body).to.have.property('error');
      });
    });

    it('returns 400 for invalid messages array', () => {
      cy.request({
        url: `${BASE_URL}/v1/chat/completions`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          messages: [],
        },
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body).to.have.property('error');
        expect(response.body.error).to.have.property('message');
      });
    });

    it('returns 400 for invalid role', () => {
      cy.request({
        url: `${BASE_URL}/v1/chat/completions`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          messages: [
            { role: 'invalid_role', content: 'test' },
          ],
        },
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body).to.have.property('error');
      });
    });

    it('returns OpenAI-compatible response for valid request (if model loaded)', () => {
      cy.request({
        url: `${BASE_URL}/v1/chat/completions`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'default',
          messages: [
            { role: 'user', content: 'Hello!' },
          ],
          max_tokens: 10,
          temperature: 0.7,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('id');
          expect(response.body).to.have.property('object', 'chat.completion');
          expect(response.body).to.have.property('created');
          expect(response.body).to.have.property('choices').to.be.an('array');
          expect(response.body).to.have.property('usage');
          expect(response.body.usage).to.have.property('prompt_tokens');
          expect(response.body.usage).to.have.property('completion_tokens');
          expect(response.body.usage).to.have.property('total_tokens');
        } else if (response.status === 503) {
          expect(response.body.error.type).to.eq('service_unavailable');
        } else {
          expect(response.status).to.be.oneOf([200, 503]);
        }
      });
    });
  });

  describe('/v1/completions', () => {
    it('returns 400 for missing prompt', () => {
      cy.request({
        url: `${BASE_URL}/v1/completions`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {},
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body).to.have.property('error');
      });
    });
  });

  describe('/v1/embeddings', () => {
    it('returns 400 for missing input', () => {
      cy.request({
        url: `${BASE_URL}/v1/embeddings`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {},
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body).to.have.property('error');
      });
    });
  });
});
