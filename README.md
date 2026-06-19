<h1 align="center">cairn</h1>

<p align="center">
<img src="https://img.shields.io/badge/C%2B%2B-23-blue?logo=c%2B%2B&logoColor=white" alt="C++23" /> <a href="https://ziglang.org/download/"><img src="https://img.shields.io/badge/Zig-0.16.0-orange?logo=zig" alt="Zig 0.16.0"/></a> <a href="LICENSE"><img src="https://img.shields.io/github/license/trevorswan11/cairn" alt="License" /></a>
</p>

<p align="center">
A standalone, single-node, client/server relational database engine
<br/>
<a href="https://github.com/trevorswan11/cairn/issues/new?labels=bug&template=bug-report.md">Report Bug</a>
&middot;
<a href="https://github.com/trevorswan11/cairn/issues/new?labels=feature&template=feature-request.md">Request Feature</a>
</p>

## About the Project

Cairn is a standalone, single-node, client/server **OLTP** relational SQL database engine aiming to compete with PostgreSQL, MySQL, InnoDB, and more!

### Built With Zig!

Zig is used as the primary orchestrator for all things cairn. All necessary build-time dependencies are fetched via the build system. For developer focused dependencies, checking out the [contributing guidelines](.github/CONTRIBUTING.md).

<details>
<summary><b>Full dependency breakdown</b></summary>

The following are "standalone" dependencies, required and manually fetched by cairn's build system.
1. [stdx](https://github.com/trevorswan11/stdx.git) is a C++ standard library and zig build system extension library that drives multiple dependencies. A full breakdown of dependencies can be found at the library's github repository. All dependencies transitively brought in by this library are open source and those and linked to `cairn` artifacts  are permissively licensed 

</details>

## Contributing

Contributions are what make the open source community such an amazing place to learn, inspire, and create. Any contributions you make are greatly appreciated.

If you have a suggestion that would make this better, please fork the repo and create a pull request. You can also simply open an issue with the tag "enhancement". Don't forget to give the project a star! Thanks again!

1. Fork the Project
2. Create your Branch (`git checkout -b some_dir/cool_thing`)
3. Commit your Changes (`git commit -m '<conventional commit>: Add some cool_thing'`)
4. Push to the Branch (`git push origin some_dir/cool_thing`)
5. Open a Pull Request

## License

Distributed under the MIT License. See `LICENSE` for more information.

## Contact

[![LinkedIn](https://img.shields.io/badge/linkedin-%230077B5.svg?style=for-the-badge&logo=linkedin&logoColor=white)](https://www.linkedin.com/in/trevorswan11/) [![Gmail](https://img.shields.io/badge/Gmail-D14836?style=for-the-badge&logo=gmail&logoColor=white)](mailto:trevor.swan@case.edu)

Project Link: [https://github.com/trevorswan11/cairn](https://github.com/trevorswan11/cairn)
