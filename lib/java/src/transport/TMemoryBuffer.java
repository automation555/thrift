// Copyright (c) 2006- Facebook
// Distributed under the Thrift Software License
//
// See accompanying file LICENSE or visit the Thrift site at:
// http://developers.facebook.com/thrift/

package com.facebook.thrift.transport;

import com.facebook.thrift.TByteArrayOutputStream;
import java.io.UnsupportedEncodingException;

/**
 * Memory buffer-based implementation of the TTransport interface.
 *
 * @author Chad Walters <chad@powerset.com>
 */
public class TMemoryBuffer extends TTransport {

  /**
   *
   */
  public TMemoryBuffer(int size) {
    arr_ = new TByteArrayOutputStream(size);
  }

  @Override
  public boolean isOpen() {
    return true;
  }

  @Override
  public void open() {
    /* Do nothing */
  }

  @Override
  public void close() {
    /* Do nothing */
  }

  @Override
  public int read(byte[] buf, int off, int len) {
    byte[] src = arr_.get();
    int amtToRead = (len > arr_.len() - pos_ ? arr_.len() - pos_ : len);
    if (amtToRead > 0) {
      System.arraycopy(src, pos_, buf, off, amtToRead);
      pos_ += amtToRead;
    }
    return amtToRead;
  }

  @Override
  public void write(byte[] buf, int off, int len) {
    arr_.write(buf, off, len);
  }

  /**
   * Output the contents of the memory buffer as a String, using the supplied
   * encoding
   * @param enc  the encoding to use
   * @return the contents of the memory buffer as a String
   */
  public String toString(String enc) throws UnsupportedEncodingException {
    return arr_.toString(enc);
  }

  // The contents of the buffer
  private TByteArrayOutputStream arr_;

  // Position to read next byte from
  private int pos_;
}

