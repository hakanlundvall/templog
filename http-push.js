(async () => {
    const payload = JSON.stringify({
        run_id: process.env.GITHUB_RUN_ID,
        commit_hash: process.env.COMMIT_HASH
    });
    console.log(`Pushing ${payload}`);
    const response = await fetch('https://www.lundvall.info/templog/firmware', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: payload
    });
    if (!response.ok) {
        throw new Error(`Failed to push: ${response.status} ${response.statusText}`);
    }
})();