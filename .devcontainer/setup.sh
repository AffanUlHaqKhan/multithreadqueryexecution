#!/bin/bash
echo "Starting Neo4j in Dev Container..."
docker-compose -f /workspace/.devcontainer/docker-compose.yml up -d
