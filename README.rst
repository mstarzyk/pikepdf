pikepdf
=======

**pikepdf** is a Python library for reading and writing PDF files. 

pikepdf is based on `QPDF <https://github.com/qpdf/qpdf>`_, a powerful PDF 
manipulation and repair library. 

Python + QPDF = "py" + "qpdf" = "pyqpdf", which looks like a dyslexia test. Say it
out loud, and it sounds like "pikepdf".

Python 3.5 and 3.6 are fully supported. 

Features:

-   Editing, manipulation and transformation of existing PDFs
-   Based on the mature, proven QPDF C++ library
-   Reading and writing encrypted PDFs, with all encryption types except public key
-   Supports all PDF compression filters
-   Supports PDF 1.3 through 1.7
-   Can create "fast web view" (linearized) PDFs
-   Creates standards compliant PDFs that pass validation in other tools
-   Automatically repairs damaged PDFs, just like QPDF
-   Can manipulate PDF/A, PDF/X and other types without losing their metadata marker
-   Implements more of the PDF specification than existing Python PDF tools
-   For convenience, renders PDF pages or embedded PDF images in Jupyter notebooks and IPython

.. code-block:: python

    # Elegant, Pythonic API
    pdf = pikepdf.open('input.pdf')
    num_pages = len(pdf.pages)
    del pdf.pages[-1]
    pdf.save('output.pdf')


pikepdf is documented and actively maintained. Commercial support is available.

This library is similar to PyPDF2 and pdfrw - it provides low level access to PDF
features and allows editing and content transformation of existing PDFs.  Some
knowledge of the PDF specification may be helpful.  It does not have the
capability to render a PDF to image.

Python 2.7 and earlier versions of Python 3 are not currently supported but 
support is probably not difficult to achieve. Pull requests are welcome.


Installation
------------

**On Unix (Linux, macOS)**

Binary wheels are available for x86-64 Linux platforms and Intel macOS. 32-bit
wheels will be added if anyone needs them.

- ``pip install pikepdf``

**From source**

A C++11 compliant compiler is required, which includes most recent versions of
GCC (4.8 and up) and clang (3.3 and up). A C++14 compiler is recommended.

libqpdf 7.0.0 is required at compile time and runtime. Many platforms have not 
updated to this version, so you may need to install this program without a
package manager.

-  clone this repository
-  install libjpeg, zlib and qpdf on your platform, including headers
-  ``pip install ./pikepdf``

**On Windows (Requires Visual Studio 2015)**

Windows is not currently part of continuous integration, so this might not work.

-  For Python 3.5:

    -  clone this repository
    -  ``pip install ./pikepdf``

pikepdf requires a C++11 compliant compiler (i.e. Visual Studio 2015 on
Windows). Running a regular ``pip install`` command will detect the
version of the compiler used to build Python and attempt to build the
extension with it. We must force the use of Visual Studio 2015.

::
    - clone this repository
    - `"%VS140COMNTOOLS%\..\..\VC\vcvarsall.bat" x64`
    - `set DISTUTILS_USE_SDK=1`
    - `set MSSdk=1`
    - `pip install ./pikepdf`

Note that this requires the user building ``pikepdf`` to have
registry edition rights on the machine, to be able to run the
``vcvarsall.bat`` script.

Windows runtime requirements
----------------------------

On Windows, the Visual C++ 2015 redistributable packages are a runtime
requirement for this project. It can be found
`here <https://www.microsoft.com/en-us/download/details.aspx?id=48145>`__.

Building the documentation
--------------------------

Documentation for the example project is generated using Sphinx. Sphinx
has the ability to automatically inspect the signatures and
documentation strings in the extension module to generate beautiful
documentation in a variety formats. The following command generates
HTML-based reference documentation; for other formats please refer to
the Sphinx manual:

-  ``cd pikepdf/docs``
-  ``make html``


License
-------

pikepdf is provided under the `Mozilla Public License 2.0 <https://www.mozilla.org/en-US/MPL/2.0/>`_
license (MPL) that can be found in the LICENSE file. By using, distributing, or
contributing to this project, you agree to the terms and conditions of this license.

`Informally <https://www.mozilla.org/en-US/MPL/2.0/FAQ/>`_, MPL 2.0 is a not a "viral" license.
It may be combined with other work, including commercial software. However, you must disclose your modifications
*to pikepdf* in source code form. In other works, fork this repository on Github or elsewhere and commit your 
contributions there, and you've satisfied the license.

The ``tests/resources/copyright`` file describes licensing terms for the test
suite and the provenance of test resources.
