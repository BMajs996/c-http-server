document.querySelector('#check').addEventListener('click', async () => {
  const status = document.querySelector('#status');
  try {
    const response = await fetch('/health');
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    status.textContent = `Server status: ${data.status}`;
  } catch (error) { status.textContent = `Health check failed: ${error.message}`; }
});
