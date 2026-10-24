# PULSAR Lobby live monitor - stdlib only, no pip deps.
FROM python:3.12-slim
WORKDIR /app
COPY monitor.py .
ENV PORT=8090
EXPOSE 8090
HEALTHCHECK --interval=30s --timeout=4s --retries=3 \
  CMD python -c "import urllib.request;urllib.request.urlopen('http://127.0.0.1:8090/healthz',timeout=3)" || exit 1
CMD ["python", "-u", "monitor.py"]
