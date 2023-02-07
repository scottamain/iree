# IREE User-Facing Documentation Website

This directory contains the source and assets for IREE's website, hosted on
[GitHub Pages](https://pages.github.com/).

The website is generated using [MkDocs](https://www.mkdocs.org/), with the
[Material for MkDocs](https://squidfunk.github.io/mkdocs-material/) theme.

## How to edit this documentation

It's easy to preview the rendered docs locally:

1. Start a Python virtual environment (optional but recommended):

    ```bash
    python3 -m venv ~/.venvs/mkdocs

    source ~/.venvs/mkdocs/bin/activate
    ```

2. Navigate to the `iree/docs/website` directory, and then
   install Material for MkDocs and other packages:

    ```bash
    pip install -r requirements.txt
    ```

3. Now start the local server:

    ```bash
    mkdocs serve
    ```

4. Open a browser to http://localhost:8000/.

That's it. The web pages automatically reload while you edit the markdown.

For more details, see https://squidfunk.github.io/mkdocs-material/getting-started/
and https://www.mkdocs.org/.

Deploy:

* This force pushes to `gh-pages` on `<your remote>`. Please don't push to the
  main repository :)

```shell
mkdocs gh-deploy --remote-name <your remote>
```
