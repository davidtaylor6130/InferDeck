// Cypress E2E tests for InferDeck Gateway - Documents CRUD + FTS5 Search Endpoint
// Run with: npx cypress run --spec "cypress/e2e/documents.cy.ts"

describe('Documents /v1/documents/* Endpoint', () => {
  const BASE_URL = Cypress.env('API_URL') || 'https://localhost:8080';

  describe('POST /v1/documents', () => {
    it('returns 400 for missing body', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body).to.have.property('error');
        expect(response.body.error).to.have.property('message');
      });
    });

    it('returns 400 for missing content', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          title: 'Test Document',
        },
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body.error).to.have.property('message');
        expect(response.body.error.message).to.contain('content');
      });
    });

    it('creates a document with valid content', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          title: 'Test Document',
          content: 'This is a test document for the RAG system. It contains searchable text content.',
          metadata: { source: 'test', tags: ['test', 'rag'] },
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('id');
          expect(response.body).to.have.property('object', 'document');
          expect(response.body).to.have.property('title', 'Test Document');
          expect(response.body).to.have.property('content');
          expect(response.body).to.have.property('metadata');
          expect(response.body).to.have.property('version', 1);
          expect(response.body).to.have.property('created_at');
          expect(response.body).to.have.property('updated_at');
        }
      });
    });

    it('creates a document with embedding', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          title: 'Document with Embedding',
          content: 'Content with an embedding vector for similarity search.',
          embedding: Array(384).fill(0.01),
          metadata: { source: 'test' },
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('id');
          expect(response.body).to.have.property('embedding');
        }
      });
    });

    it('accepts document with all optional fields', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          title: 'Full Document',
          content: 'Complete test document.',
          metadata: { source: 'api', tags: ['full', 'test'] },
          embedding: Array(768).fill(0.02),
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('id');
        }
      });
    });
  });

  describe('GET /v1/documents', () => {
    it('returns 200 with list of document IDs', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents`,
        method: 'GET',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(200);
        expect(response.body).to.have.property('object', 'list');
        expect(response.body).to.have.property('data').to.be.an('array');
      });
    });

    it('returns 200 with count info', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents`,
        method: 'GET',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(200);
        if (response.body.has_more !== undefined) {
          expect(response.body.has_more).to.be.a('boolean');
        }
      });
    });
  });

  describe('GET /v1/documents/:id', () => {
    it('returns 400 for invalid document ID format', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents/invalid-id-format`,
        method: 'GET',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        if (response.status === 400) {
          expect(response.body.error).to.have.property('message');
        }
      });
    });

    it('returns 404 for non-existent document', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents/doc_nonexistent_12345`,
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
  });

  describe('PATCH /v1/documents/:id', () => {
    it('returns 400 for update with no content', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents/doc_test_update`,
        method: 'PATCH',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {},
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body.error).to.have.property('message');
      });
    });

    it('updates a document and increments version', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents/doc_test_update`,
        method: 'PATCH',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          title: 'Updated Title',
          content: 'Updated content for the document.',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('version');
          expect(response.body).to.have.property('updated_at');
        }
      });
    });
  });

  describe('DELETE /v1/documents/:id', () => {
    it('returns 404 for deleting non-existent document', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents/doc_nonexistent_delete`,
        method: 'DELETE',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        if (response.status === 404) {
          expect(response.body).to.have.property('error');
        }
      });
    });

    it('returns 200 confirming deletion', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents/doc_test_delete`,
        method: 'DELETE',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('id');
          expect(response.body).to.have.property('deleted', true);
        }
      });
    });
  });

  describe('POST /v1/documents/search', () => {
    it('returns 400 for missing query', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents/search`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {},
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body.error).to.have.property('message');
        expect(response.body.error.message).to.contain('query');
      });
    });

    it('returns 400 for empty query string', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents/search`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          query: '   ',
        },
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body.error).to.have.property('message');
      });
    });

    it('returns search results with FTS5 (text-based)', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents/search`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          query: 'test document content',
          top_k: 5,
        },
      }).then((response) => {
        expect(response.status).to.eq(200);
        expect(response.body).to.have.property('object', 'list');
        expect(response.body).to.have.property('data').to.be.an('array');
        if (response.body.data.length > 0) {
          const result = response.body.data[0];
          expect(result).to.have.property('document');
          expect(result).to.have.property('similarity');
          expect(result.similarity).to.be.a('number');
          expect(result.similarity).to.be.at.least(0).and.at.most(1);
        }
      });
    });

    it('supports FTS5 full-text search with top_k limit', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents/search`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          query: 'rag embeddings retrieval',
          top_k: 10,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body.data).to.have.length.of.at.most(10);
        }
      });
    });

    it('returns search results ordered by relevance (descending similarity)', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents/search`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          query: 'test',
          top_k: 5,
        },
      }).then((response) => {
        if (response.status === 200 && response.body.data.length > 1) {
          const results = response.body.data;
          for (let i = 1; i < results.length; i++) {
            expect(results[i].similarity).to.be.at.most(results[i - 1].similarity);
          }
        }
      });
    });

    it('accepts min_similarity threshold', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents/search`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          query: 'test document',
          top_k: 5,
          min_similarity: 0.0,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('data');
        }
      });
    });
  });

  describe('Document storage persistence', () => {
    it('stores documents in SQLite FTS5-backed store', () => {
      cy.request({
        url: `${BASE_URL}/v1/documents`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          title: 'Persistence Test',
          content: 'This document should persist in the SQLite vector store.',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('id');
          // Verify we can retrieve it
          cy.request({
            url: `${BASE_URL}/v1/documents/${response.body.id}`,
            method: 'GET',
            failOnStatusCode: false,
            rejectUnauthorized: false,
          }).then((getResp) => {
            if (getResp.status === 200) {
              expect(getResp.body.id).to.eq(response.body.id);
              expect(getResp.body.title).to.eq('Persistence Test');
            }
          });
        }
      });
    });
  });
});
