### video server

If you want to build docker image:

```bash
docker build -t gst-pytorch 
```

Run the container:

```bash
docker run -v`pwd`:/workspace -it -p 5000:5000 rahulunair/gst-pytorch
```

Inside the container:

```bash
python server.py
```



